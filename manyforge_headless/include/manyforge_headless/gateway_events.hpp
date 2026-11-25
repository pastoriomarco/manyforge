#pragma once

// The execution host's event stream in the gateway core (EXECUTION_PATH plan
// S1/S2, OD-380): one C++ follower per process, the mirrors it keeps, the
// per-consumer subscriptions and the run wake set.
//
// - `ExecutionEvents` keeps one subscription to `execution-events.sock` in the
//   execution run root (the protocol of `execution_event_stream.v4`): it sends
//   `subscribe_events {after_cursor}`, validates every message, treats three
//   seconds without any message as a dead link and reconnects with a backoff
//   from 0.1 s doubling to 2 s (back to 0.1 s after every delivered message),
//   resuming strictly after the last delivered cursor.
// - It keeps the pushed operations table and planning jobs of every `snapshot`
//   and `state` (`PushedOperationMirror`), wakes the runs that watch an
//   operation or a planning job a message changed, and hands every message in
//   order to each subscription.
// - A subscription is a bounded FIFO and loses nothing. The shared
//   connection never waits for a consumer: a subscription it finds full (also
//   for its `disconnected`) gets `disconnected` in place of its backlog and
//   from then on its own connection, subscribed after the last message its
//   consumer took (a snapshot first), which the follower reads only while
//   that subscription has room (the host's own send limit drops a consumer
//   that stays behind; it resumes from its cursor). `Resync()` does the
//   same for one subscription on its consumer's request, from before the
//   last message taken, after a delay from 0.1 s doubling to 2 s over
//   consecutive resyncs (back to none once the consumer applied a message).
//   The other subscriptions are undisturbed.
//
// Rules: one leaf lock per mirror, per subscription, per subscriber list and
// in the wake set, each held for a memory copy; no lock across a socket call;
// the follower takes no GIL and calls no Python. One thread serves every
// connection with one poll. `Close()` ends the follower at once through an
// eventfd (a connect in progress ends within its limit).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "manyforge_headless/gateway_core.hpp"

namespace manyforge::headless::gateway {

inline constexpr std::string_view kExecutionEventsSocketFile =
    "execution-events.sock";
inline constexpr std::string_view kEventStreamApiVersion =
    "manyforge.execution_event_stream.v4";
inline constexpr std::size_t kMaximumEventStreamMessageBytes =
    4U * 1024U * 1024U;

/// The pushed copy is unavailable: no snapshot yet, or the stream was lost.
inline constexpr std::string_view kEventMirrorUnavailable =
    "execution_event_mirror_unavailable";
/// The pushed operations table predates the operation's acceptance.
inline constexpr std::string_view kEventMirrorBehind =
    "execution_event_mirror_behind";
/// A subscription's queue was full; it resumes on its own connection.
inline constexpr std::string_view kEventSubscriptionOverflow =
    "execution_event_subscription_overflow";

/// Validates one stream packet (`decode_event_stream_message`): returns the
/// message object, or throws GatewayError (`execution_event_stream_invalid`,
/// or an `error` message's own code).
[[nodiscard]] CanonicalValue DecodeEventStreamMessage(std::string_view payload);

// ---------------------------------------------------------------------------
// The run wake set (S2, D3).
// ---------------------------------------------------------------------------

/// Run ids woken since the last `Wait`, and which runs watch which operation
/// or planning job. One leaf lock; thread-safe.
class WakeSet {
 public:
  using RunId = std::uint64_t;

  void WatchOperation(RunId run_id, std::uint64_t operation_id);
  void WatchPlanningJob(RunId run_id, std::string job_id);
  /// Removes one watch of `run_id` (a pending wake stays).
  void UnwatchOperation(RunId run_id, std::uint64_t operation_id);
  void UnwatchPlanningJob(RunId run_id, const std::string &job_id);
  /// Removes every watch of `run_id` (a pending wake stays).
  void UnwatchRun(RunId run_id);
  /// Marks `run_id` woken (coalesced) from any thread.
  void Wake(RunId run_id);
  /// Returns the woken run ids (ascending) and clears them; waits until one
  /// is woken or `deadline` passes ([] then).
  [[nodiscard]] std::vector<RunId> Wait(
      std::chrono::steady_clock::time_point deadline);

  // The follower's side. Each predicate is memory work only and runs under
  // the wake lock.
  void WakeOperations(const std::function<bool(std::uint64_t)> &changed);
  void WakePlanningJobs(const std::function<bool(const std::string &)> &changed);
  /// Wakes every run with a watch (a mirror became available or unavailable).
  void WakeAllWatched();

 private:
  void WakeLocked(RunId run_id);

  std::mutex mutex_;
  std::condition_variable woken_changed_;
  std::set<RunId> woken_;
  std::map<std::uint64_t, std::set<RunId>> operations_;
  std::map<std::string, std::set<RunId>, std::less<>> planning_jobs_;
  std::map<RunId, std::pair<std::set<std::uint64_t>, std::set<std::string>>>
      by_run_;
};

// ---------------------------------------------------------------------------
// The pushed operations table and planning jobs (OD-213, OD-213 (1)).
// ---------------------------------------------------------------------------

/// The stream's `state.operations`, `state.active_tcp_baseline` and
/// `state.planning_jobs`, replaced whole by every `snapshot` and `state`;
/// unavailable from a lost stream until the next snapshot. One leaf lock held
/// for a pointer copy; the tables themselves are immutable.
class PushedOperationMirror {
 public:
  struct Table {
    std::uint64_t change{};
    std::unordered_map<std::uint64_t, CanonicalValue> entries;
    /// `active_tcp_baseline`, null when the state has none.
    CanonicalValue active_tcp;
    std::unordered_map<std::string, std::string> planning_jobs;
  };

  /// One `snapshot` or `state` object. A malformed table or planning-job
  /// list makes the copy unavailable and throws GatewayError
  /// (`execution_event_stream_invalid`). Returns the table before and after.
  std::pair<std::shared_ptr<const Table>, std::shared_ptr<const Table>>
  ApplyState(const CanonicalValue &state);
  /// Unavailable, never stale, until the next snapshot. Returns the table
  /// that was current.
  std::shared_ptr<const Table> StreamLost();

  /// The operation as a `read` reply shows it ({ok, command: "read",
  /// operation, resident: true, last_control, control_pending, active_tcp}).
  /// Throws GatewayError `execution_event_mirror_unavailable` while the copy
  /// is unavailable and `execution_event_mirror_behind` while its `change` is
  /// below `after_change`; nullopt when the current copy cannot show it
  /// (retired, beyond the list's bound, or listed without its state).
  [[nodiscard]] std::optional<CanonicalValue> Envelope(
      std::uint64_t operation_id, std::uint64_t after_change) const;
  /// The job's state; nullopt while the copy does not list it. Throws
  /// `execution_event_mirror_unavailable` while the copy is unavailable.
  [[nodiscard]] std::optional<std::string> PlanningJobState(
      std::string_view job_id) const;
  [[nodiscard]] std::shared_ptr<const Table> Current() const;

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const Table> table_;
};

// ---------------------------------------------------------------------------
// Subscriptions and the follower.
// ---------------------------------------------------------------------------

using EventMessage = std::shared_ptr<const CanonicalValue>;

/// The follower's wake-up (an eventfd): a subscription raises it on a resync,
/// a close, or room in a full queue. Shared, so a subscription may outlive
/// its follower.
class FollowerSignal {
 public:
  /// Throws GatewayError (`execution_owner_unavailable`) without an eventfd.
  FollowerSignal();
  ~FollowerSignal();
  FollowerSignal(const FollowerSignal &) = delete;
  FollowerSignal &operator=(const FollowerSignal &) = delete;
  void Raise() noexcept;
  void Clear() noexcept;
  [[nodiscard]] int fd() const noexcept { return fd_; }

 private:
  int fd_;
};

/// One consumer's ordered messages. Thread-safe; `Next` waits with no other
/// lock held. It keeps the stream cursor after the last message taken and
/// after the one before it (a consumer calling `Next` again has applied the
/// last one), and counts the resyncs since the consumer last applied a
/// message.
class EventSubscription {
 public:
  EventSubscription(std::size_t capacity, std::shared_ptr<FollowerSignal> signal,
                    std::uint64_t cursor);
  /// The next message; nullptr after `timeout` or once closed.
  [[nodiscard]] EventMessage Next(std::chrono::nanoseconds timeout);
  /// Ends the subscription; a waiting `Next` returns at once.
  void Close();
  /// The last message taken counts as not applied: the queue becomes one
  /// `disconnected`, followed by a fresh snapshot and the messages after the
  /// cursor before that message, on this subscription's own connection,
  /// which opens after the resync delay. The other subscriptions are
  /// undisturbed.
  void Resync();

  // The follower's side.
  enum class PushResult { kPushed, kFull, kStale };
  /// Appends `message` from the feed of `epoch` (its stream cursor when it
  /// carries one). kFull: nothing appended; kStale: the subscription has
  /// moved to another feed. A closed subscription takes and drops it.
  PushResult Push(EventMessage message, std::optional<std::uint64_t> cursor,
                  std::uint64_t epoch);
  /// Moves to its own feed: the queue becomes `disconnected`, the feed starts
  /// after the last message taken.
  void Detach(EventMessage disconnected);
  struct Feed {
    std::uint64_t epoch;
    std::uint64_t after_cursor;
    /// The consecutive resyncs that started this feed (0: not a resync).
    std::uint32_t resyncs;
  };
  /// The current feed: epoch 0 is the shared connection.
  [[nodiscard]] Feed CurrentFeed() const;
  [[nodiscard]] bool HasRoom() const;
  [[nodiscard]] bool closed() const noexcept { return closed_.load(); }
  [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_.load(); }

 private:
  struct Entry {
    EventMessage message;
    std::uint64_t cursor;
  };
  void RestartLocked(EventMessage disconnected, std::uint64_t after_cursor,
                     std::uint32_t resyncs);

  const std::size_t capacity_;
  const std::shared_ptr<FollowerSignal> signal_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Entry> queue_;
  std::uint64_t pushed_cursor_;   // After the last entry.
  std::uint64_t taken_cursor_;    // After the last message taken.
  std::uint64_t applied_cursor_;  // Before the last message taken.
  std::uint64_t feed_cursor_;     // Where the current feed starts.
  std::uint32_t resyncs_{0};      // Since the consumer last applied a message.
  std::uint32_t feed_resyncs_{0}; // `resyncs_` when this feed started.
  bool last_taken_applies_{false};  // A stream message, not resynced.
  // Written under `mutex_`; read without it by the subscriber list.
  std::atomic<std::uint64_t> epoch_{0};
  std::atomic<bool> closed_{false};
};

struct ExecutionEventsOptions {
  /// No message at all for this long is a dead link (three missed heartbeats).
  std::chrono::milliseconds dead_link{3000};
  std::chrono::milliseconds backoff_initial{100};
  std::chrono::milliseconds backoff_maximum{2000};
  std::chrono::milliseconds connect_limit{3000};
  std::size_t subscription_capacity{1024};
};

class ExecutionEvents {
 public:
  /// Pins `execution_run_root` (throws GatewayError like PinnedRunRoot).
  explicit ExecutionEvents(std::filesystem::path execution_run_root,
                           std::shared_ptr<WakeSet> wake_set = nullptr,
                           ExecutionEventsOptions options = {});
  ~ExecutionEvents();
  ExecutionEvents(const ExecutionEvents &) = delete;
  ExecutionEvents &operator=(const ExecutionEvents &) = delete;

  /// Starts the follower thread (once; later calls do nothing).
  void Start();
  /// Ends the follower at once and closes every subscription.
  void Close();

  /// Every decoded message from now on, in order, plus
  /// {"type": "disconnected", "code", "message"} at each link loss (a new
  /// snapshot follows, then the messages after the last one taken).
  [[nodiscard]] std::shared_ptr<EventSubscription> Subscribe();

  /// True from a connection's snapshot until that connection is lost.
  [[nodiscard]] bool Connected() const noexcept;

  [[nodiscard]] std::optional<CanonicalValue> OperationEnvelope(
      std::uint64_t operation_id, std::uint64_t after_change) const {
    return operations_.Envelope(operation_id, after_change);
  }
  [[nodiscard]] std::optional<std::string> PlanningJobState(
      std::string_view job_id) const {
    return operations_.PlanningJobState(job_id);
  }

 private:
  struct LinkFailure {
    std::string code;
    std::string message;
  };
  /// One connection and its reconnect schedule (follower thread only).
  struct Link;
  /// A subscription on its own connection.
  struct OwnFeed;

  void Run();
  /// Connects and subscribes after `link.cursor`.
  std::optional<LinkFailure> Open(Link &link);
  /// Receives one packet from a readable link (`message` stays null when
  /// there was none).
  std::optional<LinkFailure> Receive(Link &link, EventMessage &message);
  /// Applies one message of the shared connection and hands it out.
  void Deliver(Link &link, const EventMessage &message);
  void FeedOwn(OwnFeed &feed, const EventMessage &message);
  void SharedLost(Link &link, const LinkFailure &failure);
  void OwnLost(OwnFeed &feed, const LinkFailure &failure);
  /// Prunes closed subscriptions and starts the own feeds that a detach or a
  /// resync asked for.
  void Rescan(std::vector<OwnFeed> &own);
  /// The subscriptions on the shared connection.
  [[nodiscard]] std::vector<std::shared_ptr<EventSubscription>> Subscribers();

  PinnedRunRoot root_;
  std::shared_ptr<WakeSet> wake_set_;
  ExecutionEventsOptions options_;
  PushedOperationMirror operations_;
  std::shared_ptr<FollowerSignal> signal_;
  int stop_fd_{-1};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> connected_{false};
  std::mutex lifecycle_mutex_;
  std::thread follower_;
  bool started_{false};

  std::mutex subscribers_mutex_;
  /// Every open subscription (epoch 0: on the shared connection).
  std::vector<std::shared_ptr<EventSubscription>> subscribers_;

  /// The shared connection's cursor (a new subscription starts there).
  std::atomic<std::uint64_t> cursor_{0};
  // Follower thread only.
  std::vector<char> receive_buffer_;
};

}  // namespace manyforge::headless::gateway
