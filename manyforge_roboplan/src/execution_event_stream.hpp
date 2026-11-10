#pragma once

// Event subscription stream of the execution host.
//
// One private SEQPACKET socket (`execution-events.sock` in the run root), one
// subscription per connection, one writer thread per subscriber. Each writer
// reads the operation journal from its own cursor, so the journal itself is
// the subscriber's bounded queue (one page in memory; no in-memory queue,
// maintainer-approved): a slow subscriber only lags, never loses an event, and
// never holds a lock while it sends. Command, Stop and control lanes share
// nothing with it but the same short reads the `events` command already makes.
//
// Client -> host, one packet:
//   {"after_cursor": <int>, "api_version": "manyforge.execution_command.v3",
//    "command": "subscribe_events", "owner_epoch": <str|null>}
// Host -> client, one packet per message, each
//   {"api_version": "manyforge.execution_event_stream.v1", "type": ...}:
//   snapshot  {owner_epoch, cursor, reason, state}  first message, and again
//             after a gap; events continue strictly after `cursor`.
//   event     {owner_epoch, event}   one journal row as in an `events` page.
//   state     {owner_epoch, state}   the non-journal facts, when they changed.
//   progress  {owner_epoch, operation_progress}  the `events` reply's progress
//             rows, when they changed (also to []).
//   heartbeat {owner_epoch, cursor}  every second.
//   gap       {owner_epoch, reason}  the subscriber's cursor cannot be served
//             (`owner_epoch_changed`); a snapshot follows.
//   error     {error}                the subscription ends.
// `state` is the `events` reply without its page and progress:
// admission_generation, carried_object_lost_signals, execution_owner_ready,
// interrupt_rule_catalog, interrupt_rule_state_baseline, runtime_faults.
// Each round sends the new rows first, then a changed state, so a ready
// baseline's `through_sequence` covers every rule-state row sent before it,
// then changed progress. A writer wakes on the change signal (journal commit,
// rule-state cut, catalog install, carried-object-lost store) and for the
// heartbeat; while an operation executes it also re-reads progress every
// `kProgressPeriod` (progress is sampled at control rate in lock-free
// publications, which never take the signal's lock).

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/execution_event_change_signal.hpp>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {

inline constexpr std::string_view kEventStreamApiVersion =
    "manyforge.execution_event_stream.v1";

/// The two reads a stream makes. Both may throw; the subscription then ends
/// with an `error` message and the client reconnects.
struct ExecutionEventStreamSource final {
  using Value = content_identity::CanonicalValue;
  /// Current owner epoch and state object.
  std::function<std::pair<std::string, Value>()> read_state;
  /// One `events` page object (`events`, `next_after_sequence`, `has_more`)
  /// of `owner_epoch` strictly after `after`.
  std::function<Value(const std::string &owner_epoch, std::uint64_t after)>
      read_page;
  /// The executing operations' progress rows (an array; empty when none).
  std::function<Value()> read_progress;
};

class ExecutionEventStream final {
public:
  using Value = content_identity::CanonicalValue;
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::milliseconds kHeartbeatPeriod{1000};
  /// Progress re-read period while an operation executes (the former GUI
  /// event-pump cadence).
  static constexpr std::chrono::milliseconds kProgressPeriod{100};
  /// A subscriber that accepts no packet for this long is dropped (it
  /// reconnects with its cursor).
  static constexpr std::chrono::milliseconds kSendDeadline{3000};
  static constexpr std::chrono::milliseconds kSubscribeDeadline{100};
  static constexpr std::size_t kMaximumSubscribers = 8U;
  /// Pages one round sends before it re-reads the state (it continues at
  /// once); bounds the staleness of `state` during a long catch-up.
  static constexpr std::size_t kMaximumPagesPerRound = 16U;
  static constexpr std::size_t kMaximumMessageBytes = 4U * 1024U * 1024U;

  explicit ExecutionEventStream(
      ExecutionEventStreamSource source,
      core::ExecutionEventChangeSignal &signal =
          core::execution_event_change_signal())
      : source_(std::move(source)), signal_(signal) {}

  ExecutionEventStream(const ExecutionEventStream &) = delete;
  ExecutionEventStream &operator=(const ExecutionEventStream &) = delete;

  /// Accepts subscribers on the listening `socket` until `stop`, then ends
  /// every subscription (each writer sees the stop within one poll slice)
  /// and returns once they are closed.
  void Run(const int socket, const std::stop_token stop) noexcept {
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
    std::list<Writer> writers;
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
      if (client.get() < 0)
        continue;
      writers.remove_if([](const Writer &writer) { return writer.done->load(); });
      ucred peer{};
      socklen_t peer_size = sizeof(peer);
      if (::getsockopt(client.get(), SOL_SOCKET, SO_PEERCRED, &peer,
                       &peer_size) != 0 ||
          peer_size != sizeof(peer) || peer.uid != ::geteuid())
        continue;
      if (writers.size() >= kMaximumSubscribers) {
        static_cast<void>(Send(client.get(),
                               Error("execution_event_subscribers_full",
                                     "the execution host serves its bounded "
                                     "number of event subscriptions"),
                               stop));
        continue;
      }
      auto done = std::make_shared<std::atomic<bool>>(false);
      writers.push_back(Writer{
          done, std::jthread([this, done, fd = std::make_shared<Fd>(std::move(client))](
                                 const std::stop_token writer_stop) {
            Serve(fd->get(), writer_stop);
            done->store(true);
          })});
    }
    // Each jthread is stopped and joined; its socket closes with it.
    writers.clear();
  }

  /// Serves one connected subscriber until it fails, disconnects or `stop`.
  void Serve(const int client, const std::stop_token stop) noexcept {
    try {
      const auto request = ReadSubscribe(client);
      if (!request) {
        static_cast<void>(Send(client,
                               Error("execution_event_subscription_invalid",
                                     "subscribe_events request is malformed"),
                               stop));
        return;
      }
      auto [epoch, state] = source_.read_state();
      std::uint64_t cursor = request->after_cursor;
      std::string reason = "subscribed";
      if (request->owner_epoch && *request->owner_epoch != epoch) {
        if (!Send(client, Gap(epoch, "owner_epoch_changed"), stop))
          return;
        cursor = 0U;
        reason = "owner_epoch_changed";
      }
      if (!Send(client, Snapshot(epoch, cursor, reason, state), stop))
        return;
      auto sent_state = content_identity::canonical_json(state);
      std::string sent_progress = "[]";
      auto next_heartbeat = Clock::now() + kHeartbeatPeriod;
      while (!stop.stop_requested()) {
        const auto generation = signal_.Generation();
        bool more = false;
        for (std::size_t index = 0; index < kMaximumPagesPerRound; ++index) {
          const auto page = source_.read_page(epoch, cursor);
          const auto *events = page.find_object_member("events");
          const auto *has_more = page.find_object_member("has_more");
          if (events == nullptr || !events->is_array() || has_more == nullptr ||
              !has_more->is_bool())
            throw std::runtime_error("event page is malformed");
          for (const auto &event : events->as_array()) {
            const auto sequence = Counter(event, "sequence");
            if (!sequence || *sequence <= cursor)
              throw std::runtime_error("event page is not ordered");
            if (!Send(client, Message("event", epoch, {{"event", event}}), stop))
              return;
            cursor = *sequence;
          }
          more = has_more->as_bool();
          if (!more)
            break;
        }
        auto [current_epoch, current_state] = source_.read_state();
        if (current_epoch != epoch) {
          // A new Kernel owner in this process: its journal starts anew.
          epoch = std::move(current_epoch);
          cursor = 0U;
          if (!Send(client, Gap(epoch, "owner_epoch_changed"), stop) ||
              !Send(client, Snapshot(epoch, cursor, "owner_epoch_changed",
                                     current_state),
                    stop))
            return;
          sent_state = content_identity::canonical_json(current_state);
          continue;
        }
        auto current = content_identity::canonical_json(current_state);
        if (current != sent_state) {
          if (!Send(client,
                    Message("state", epoch, {{"state", std::move(current_state)}}),
                    stop))
            return;
          sent_state = std::move(current);
        }
        const auto progress =
            source_.read_progress ? source_.read_progress() : Value(Value::Array{});
        if (!progress.is_array())
          throw std::runtime_error("operation progress is not an array");
        auto progress_json = content_identity::canonical_json(progress);
        if (progress_json != sent_progress) {
          if (!Send(client,
                    Message("progress", epoch, {{"operation_progress", progress}}),
                    stop))
            return;
          sent_progress = std::move(progress_json);
        }
        if (Clock::now() >= next_heartbeat) {
          if (!Send(client,
                    Message("heartbeat", epoch,
                            {{"cursor", Value(static_cast<double>(cursor))}}),
                    stop))
            return;
          next_heartbeat = Clock::now() + kHeartbeatPeriod;
        }
        if (!more)
          signal_.WaitAfter(generation,
                            progress.as_array().empty()
                                ? next_heartbeat
                                : std::min(next_heartbeat,
                                           Clock::now() + kProgressPeriod),
                            stop);
      }
    } catch (const std::exception &error) {
      static_cast<void>(
          Send(client, Error("execution_event_stream_failed", error.what()), stop));
    } catch (...) {
      static_cast<void>(Send(
          client, Error("execution_event_stream_failed", "event stream read threw"),
          stop));
    }
  }

private:
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

  struct Writer final {
    std::shared_ptr<std::atomic<bool>> done;
    std::jthread thread;
  };

  struct Subscribe final {
    std::uint64_t after_cursor{};
    std::optional<std::string> owner_epoch;
  };

  [[nodiscard]] static std::optional<std::uint64_t>
  Counter(const Value &object, const std::string_view name) {
    const auto *value = object.find_object_member(name);
    if (value == nullptr || !value->is_number())
      return std::nullopt;
    const double number = value->as_number();
    if (number < 0.0 || number > 9'007'199'254'740'991.0 ||
        std::floor(number) != number)
      return std::nullopt;
    return static_cast<std::uint64_t>(number);
  }

  [[nodiscard]] static std::optional<Subscribe> ReadSubscribe(const int client) {
    pollfd inbound{client, POLLIN, 0};
    int ready = -1;
    do {
      ready = ::poll(&inbound, 1, static_cast<int>(kSubscribeDeadline.count()));
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || (inbound.revents & POLLIN) == 0)
      return std::nullopt;
    std::string buffer(4096U, '\0');
    ssize_t count = -1;
    do {
      count = ::recv(client, buffer.data(), buffer.size(), MSG_TRUNC);
    } while (count < 0 && errno == EINTR);
    if (count <= 0 || static_cast<std::size_t>(count) > buffer.size())
      return std::nullopt;
    buffer.resize(static_cast<std::size_t>(count));
    try {
      const auto root = content_identity::parse_canonical_json(buffer);
      const auto *api = root.find_object_member("api_version");
      const auto *command = root.find_object_member("command");
      const auto *epoch = root.find_object_member("owner_epoch");
      const auto after = Counter(root, "after_cursor");
      if (!root.is_object() || root.as_object().size() != 4U || api == nullptr ||
          !api->is_string() ||
          api->as_string() != "manyforge.execution_command.v3" ||
          command == nullptr || !command->is_string() ||
          command->as_string() != "subscribe_events" || epoch == nullptr ||
          !(epoch->is_null() ||
            (epoch->is_string() && !epoch->as_string().empty() &&
             epoch->as_string().size() <= 2048U)) ||
          !after)
        return std::nullopt;
      return Subscribe{*after, epoch->is_null()
                                   ? std::nullopt
                                   : std::optional<std::string>(epoch->as_string())};
    } catch (...) {
      return std::nullopt;
    }
  }

  [[nodiscard]] static Value Message(const std::string_view type,
                                     const std::string &epoch,
                                     Value::Object fields) {
    fields.emplace_back("api_version", Value(std::string(kEventStreamApiVersion)));
    fields.emplace_back("owner_epoch", Value(epoch));
    fields.emplace_back("type", Value(std::string(type)));
    return Value(std::move(fields));
  }

  [[nodiscard]] static Value Snapshot(const std::string &epoch,
                                      const std::uint64_t cursor,
                                      const std::string &reason,
                                      const Value &state) {
    return Message("snapshot", epoch,
                   {{"cursor", Value(static_cast<double>(cursor))},
                    {"reason", Value(reason)},
                    {"state", state}});
  }

  [[nodiscard]] static Value Gap(const std::string &epoch,
                                 const std::string &reason) {
    return Message("gap", epoch, {{"reason", Value(reason)}});
  }

  [[nodiscard]] static Value Error(const std::string_view code,
                                   const std::string_view message) {
    return Value(Value::Object{
        {"api_version", Value(std::string(kEventStreamApiVersion))},
        {"error", Value(Value::Object{
                      {"code", Value(std::string(code))},
                      {"details", Value(Value::Object{})},
                      {"message", Value(std::string(message.substr(0U, 1024U)))}})},
        {"type", Value("error")}});
  }

  /// One packet, or false: the subscriber is gone, too slow or stopping.
  [[nodiscard]] static bool Send(const int client, const Value &message,
                                 const std::stop_token stop) noexcept {
    try {
      const auto bytes = content_identity::canonical_json(message);
      if (bytes.empty() || bytes.size() > kMaximumMessageBytes)
        return false;
      const auto deadline = Clock::now() + kSendDeadline;
      for (;;) {
        const auto sent = ::send(client, bytes.data(), bytes.size(),
                                 MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent == static_cast<ssize_t>(bytes.size()))
          return true;
        if (sent >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
          return false;
        if (stop.stop_requested() || Clock::now() >= deadline)
          return false;
        // A full socket buffer: wait for room in short slices so a stop is
        // seen promptly; nothing else waits on this writer.
        pollfd outbound{client, POLLOUT, 0};
        static_cast<void>(::poll(&outbound, 1, 100));
      }
    } catch (...) {
      return false;
    }
  }

  ExecutionEventStreamSource source_;
  core::ExecutionEventChangeSignal &signal_;
};

} // namespace manyforge::roboplan_backend::host_detail
