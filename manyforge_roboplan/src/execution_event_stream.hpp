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
//   {"after_cursor": <int>, "api_version": "manyforge.execution_command.v4",
//    "command": "subscribe_events"}
// Host -> client, one packet per message, each
//   {"api_version": "manyforge.execution_event_stream.v4", "type": ...}:
//   snapshot  {cursor, reason, state}  first message; events continue
//             strictly after `cursor`.
//   event     {event}                one journal row as in an `events` page.
//   state     {state}                the non-journal facts, when they changed.
//   progress  {operation_progress}   the `events` reply's progress rows, when
//             they changed (also to []).
//   heartbeat {cursor}               every second.
//   error     {error}                the subscription ends.
// One Kernel and one journal per application start: no message names an
// owner (contract 1.16.4 rule 1).
// `state` is the `events` reply without its page and progress:
// admission_generation, carried_object_lost_signals, execution_owner_ready,
// interrupt_rule_catalog, interrupt_rule_state_baseline, runtime_faults;
// and the Kernel's memory Stop facts (stop state model K18):
// `program_run_hold` (null without a claimed run, else {program_run_id,
// hold_revision, frozen, frozen_by|null, all_groups_held, held_by_rule|null}),
// `last_stop_event_id` (null before the first Stop of this start) and
// `stop_progress_sequence`. The Kernel raises the change signal after every
// change of these facts (hold change, release, new event, a participant
// ending a stop attempt), so a Stop reaches the runner without waiting for
// the journal.
// v3 (OD-215): `runtime_faults.operation_journal_write_fault` (when not
// null) also carries `queue_full` (bool), the journal queue's "full" level;
// it changes without a new row, and the change signal or the next heartbeat
// round carries it.
// v3 (OD-205 (b)): `operations` {change, faulted[], faulted_total}, from
// Kernel memory: `change` rises at every change of an operation's state,
// residence or pending control; `faulted` lists, lowest operation ids first
// and at most 32, the resident operations in state failed, interrupted or
// intervention_required or stopped by the runtime monitor, each with the
// facts its alarm reads ({operation_id, operation_revision, state,
// stop_event_id, pending_control, runtime_monitor_stop, complete} and, when
// `complete`, {program_binding_id, program_run_id, resources (<= 16),
// resource_total, terminal_error {code, reason, message (<= 256 bytes),
// message_truncated}, participants [{state, execution_scope, terminal_error
// {reason}, terminal_fact {kind, effect, error}}]}); `faulted_total` counts
// them all. The Kernel raises the change signal after the unlock of every
// such change.
// v4 (OD-213): `operations` also carries `resident` and `resident_total`:
// every resident operation (the active set and the takeover slots), lowest
// operation ids first and at most 64, each {operation_id, operation,
// control_pending, last_control} with the `read` reply's members (its
// `operation` without the participants' `progress` and the `updated_at_ns`;
// null when it encodes to more than 16 KiB); `resident_total` counts them
// all. `change` and both lists are read in one Kernel section, so a reader
// whose table has a `change` at least an acceptance reply's
// `operations_change` sees that operation as it was after the acceptance, or
// sees it retired. The runner's nodes read their operations from it.
// v4 (OD-213 (1)): `planning_jobs`, the execution host's pick/place planning
// jobs in acceptance order, at most 8, each {job_id, kind ("pick"|"place"),
// program_run_id|null, state ("accepted"|"planning"|"done"|"failed"|
// "cancelled")}; never the result, which `read_planning_job` returns. The
// host raises the change signal after every change of the job table.
// Each round sends a changed state first, then the new rows, then changed
// progress. The rule-state baseline in `state` is read from Kernel memory:
// a rule trip that starts and ends between two state reads sends nothing.
// A writer wakes on the change signal (journal commit, rule-state change,
// catalog install, carried-object-lost store, Stop facts, operation changes)
// and for the
// heartbeat; while an operation executes it also re-reads progress every
// `kProgressPeriod` (progress is sampled at control rate in lock-free
// publications, which never take the signal's lock).

// The descriptor holder, the send with deadline, the error frame, the peer
// check, the subscriber pool and the acceptor are the shared private stream
// server (`manyforge_private_stream`); this stream passes its own bounds and
// codes.

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/execution_event_change_signal.hpp>
#include <manyforge_private_stream/private_stream_server.hpp>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {

inline constexpr std::string_view kEventStreamApiVersion =
    "manyforge.execution_event_stream.v4";

/// The two reads a stream makes. Both may throw; the subscription then ends
/// with an `error` message and the client reconnects.
struct ExecutionEventStreamSource final {
  using Value = content_identity::CanonicalValue;
  /// The current state object.
  std::function<Value()> read_state;
  /// One `events` page object (`events`, `next_after_sequence`, `has_more`)
  /// strictly after `after`.
  std::function<Value(std::uint64_t after)> read_page;
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
    private_stream::AcceptSubscribers(
        socket,
        private_stream::SubscriberPoolOptions{
            kMaximumSubscribers, std::string(kEventStreamApiVersion),
            "execution_event_subscribers_full",
            "the execution host serves its bounded number of event "
            "subscriptions",
            kMaximumMessageBytes, kSendDeadline,
            // A refused subscriber's subscribe packet is read first, within
            // the subscribe limit, so the "full" error reaches it.
            kSubscribeDeadline},
        [this](const int subscriber, const std::stop_token writer_stop) {
          Serve(subscriber, writer_stop);
        },
        stop);
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
      auto state = source_.read_state();
      std::uint64_t cursor = request->after_cursor;
      if (!Send(client, Snapshot(cursor, "subscribed", state), stop))
        return;
      auto sent_state = content_identity::canonical_json(state);
      std::string sent_progress = "[]";
      auto next_heartbeat = Clock::now() + kHeartbeatPeriod;
      while (!stop.stop_requested()) {
        const auto generation = signal_.Generation();
        // State first (OD-205 (b)): a slow journal-file read never delays
        // the Kernel's memory facts; no consumer orders rows before it.
        auto current_state = source_.read_state();
        auto current = content_identity::canonical_json(current_state);
        if (current != sent_state) {
          if (!Send(client,
                    Message("state", {{"state", std::move(current_state)}}),
                    stop))
            return;
          sent_state = std::move(current);
        }
        bool more = false;
        for (std::size_t index = 0; index < kMaximumPagesPerRound; ++index) {
          const auto page = source_.read_page(cursor);
          const auto *events = page.find_object_member("events");
          const auto *has_more = page.find_object_member("has_more");
          if (events == nullptr || !events->is_array() || has_more == nullptr ||
              !has_more->is_bool())
            throw std::runtime_error("event page is malformed");
          for (const auto &event : events->as_array()) {
            const auto sequence = Counter(event, "sequence");
            if (!sequence || *sequence <= cursor)
              throw std::runtime_error("event page is not ordered");
            if (!Send(client, Message("event", {{"event", event}}), stop))
              return;
            cursor = *sequence;
          }
          more = has_more->as_bool();
          if (!more)
            break;
        }
        const auto progress =
            source_.read_progress ? source_.read_progress() : Value(Value::Array{});
        if (!progress.is_array())
          throw std::runtime_error("operation progress is not an array");
        auto progress_json = content_identity::canonical_json(progress);
        if (progress_json != sent_progress) {
          if (!Send(client,
                    Message("progress", {{"operation_progress", progress}}),
                    stop))
            return;
          sent_progress = std::move(progress_json);
        }
        if (Clock::now() >= next_heartbeat) {
          if (!Send(client,
                    Message("heartbeat",
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
  using Fd = private_stream::Fd;

  struct Subscribe final {
    std::uint64_t after_cursor{};
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
      const auto after = Counter(root, "after_cursor");
      if (!root.is_object() || root.as_object().size() != 3U || api == nullptr ||
          !api->is_string() ||
          api->as_string() != "manyforge.execution_command.v4" ||
          command == nullptr || !command->is_string() ||
          command->as_string() != "subscribe_events" || !after)
        return std::nullopt;
      return Subscribe{*after};
    } catch (...) {
      return std::nullopt;
    }
  }

  [[nodiscard]] static Value Message(const std::string_view type,
                                     Value::Object fields) {
    fields.emplace_back("api_version", Value(std::string(kEventStreamApiVersion)));
    fields.emplace_back("type", Value(std::string(type)));
    return Value(std::move(fields));
  }

  [[nodiscard]] static Value Snapshot(const std::uint64_t cursor,
                                      const std::string &reason,
                                      const Value &state) {
    return Message("snapshot",
                   {{"cursor", Value(static_cast<double>(cursor))},
                    {"reason", Value(reason)},
                    {"state", state}});
  }

  [[nodiscard]] static Value Error(const std::string_view code,
                                   const std::string_view message) {
    return private_stream::ErrorFrame(kEventStreamApiVersion, code, message);
  }

  /// One packet, or false: the subscriber is gone, too slow or stopping.
  [[nodiscard]] static bool Send(const int client, const Value &message,
                                 const std::stop_token stop) noexcept {
    return private_stream::SendFrame(client, message, kMaximumMessageBytes,
                                     kSendDeadline, stop);
  }

  ExecutionEventStreamSource source_;
  core::ExecutionEventChangeSignal &signal_;
};

} // namespace manyforge::roboplan_backend::host_detail
