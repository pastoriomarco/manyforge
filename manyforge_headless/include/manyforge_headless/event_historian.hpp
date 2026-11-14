#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/typed_error.hpp>

#include <manyforge_headless/operator_alarm_owner.hpp>

namespace manyforge::headless {

/// v3 (stop state model): a Stop is recorded as the event kinds
/// `stop_event_accepted` and `stop_event_completed` with the
/// `manyforge.stop_event.v1` payload; pages, summaries and details carry
/// those events and are bumped with them. Each page and summary-page row
/// names the application start it belongs to (`application_start_id`).
inline constexpr std::string_view kHistorianEventSchemaVersion =
    "manyforge.historian_event.v3";
inline constexpr std::string_view kHistorianPageSchemaVersion =
    "manyforge.historian_page.v3";
inline constexpr std::string_view kHistorianSummaryPageSchemaVersion =
    "manyforge.historian_summary_page.v3";
inline constexpr std::string_view kHistorianEventSummarySchemaVersion =
    "manyforge.historian_event_summary.v3";
inline constexpr std::string_view kHistorianDetailSchemaVersion =
    "manyforge.historian_detail.v3";
inline constexpr std::string_view kHistorianCapturePageSchemaVersion =
    "manyforge.historian_capture_page.v1";
/// Program diagnostic versions this History file holds. Only their
/// producer-level identity fields are correlation attribution; blackboard
/// values are data. History opens only its own format version, so no earlier
/// diagnostic is ever read from it.
inline constexpr std::array<std::string_view, 1>
    kProgramDiagnosticSchemaVersions{"manyforge.program_diagnostic.v5"};
inline constexpr std::size_t kDefaultHistorianQueueCapacity = 1024U;
/// Minimum interval between the starts of two writer commits. Events that
/// arrive meanwhile wait in memory and share the next transaction.
inline constexpr std::chrono::milliseconds kDefaultHistorianCommitGap{50};
/// Upper bound of events in one writer transaction.
inline constexpr std::size_t kMaximumHistorianBatchEvents = 128U;
inline constexpr std::size_t kDefaultHistorianRecordRetention = 100000U;
inline constexpr std::size_t kMaximumHistorianPageSize = 10000U;
inline constexpr std::size_t kMaximumHistorianPayloadBytes = 256U * 1024U;

class EventHistorianError : public manyforge::core::TypedError {
public:
  EventHistorianError(std::string code, std::string message);
  [[nodiscard]] const std::string &code() const noexcept;
};

struct HistorianEvent final {
  std::string event_id;
  std::string component;
  std::string event_type;
  std::string severity;
  std::int64_t occurred_at_ns;
  std::string occurred_at_wall;
  manyforge::content_identity::CanonicalValue payload;
};

[[nodiscard]] HistorianEvent
MakeHistorianEvent(std::string component, std::string event_type,
                   std::string severity, std::int64_t occurred_at_ns,
                   std::string occurred_at_wall,
                   manyforge::content_identity::CanonicalValue payload);
[[nodiscard]] HistorianEvent
MakeAlarmLifecycleHistorianEvent(const AlarmLifecycleFact &fact);
[[nodiscard]] manyforge::content_identity::CanonicalValue
HistorianEventValue(const HistorianEvent &event);
/// Exact persisted digest form. Values written before the v2 wire cutover keep
/// their v1 numeric representation so an existing database remains readable.
[[nodiscard]] manyforge::content_identity::CanonicalValue
HistorianPersistenceValue(const HistorianEvent &event);

struct HistorianStoredEvent final {
  std::uint64_t sequence;
  HistorianEvent event;
  std::optional<std::int64_t> occurred_at_utc_ns{};
  std::optional<std::string> clock_owner{};
  bool wall_clock_regressed{};
  /// The application start the row belongs to (16 hex).
  std::string application_start_id{};
};

/// Strict recorded ISO-8601 wall time, never a freshness or causal clock.
/// Unknown, leap-second or out-of-int64 historical times remain unavailable.
[[nodiscard]] std::optional<std::int64_t>
HistorianRecordedUtcNs(std::string_view wall) noexcept;
[[nodiscard]] std::string HistorianConditionSeverity(const HistorianEvent &event);

struct HistorianQuery final {
  std::optional<std::int64_t> start_ns;
  std::optional<std::int64_t> end_ns;
  std::set<std::string> components;
  std::set<std::string> event_types;
  std::optional<std::uint64_t> before_sequence;
  std::size_t limit{1000U};
  std::optional<std::int64_t> start_utc_ns{};
  std::optional<std::int64_t> end_utc_ns{};
  std::set<std::string> severities{};
  std::map<std::string, std::string> correlations{};
  std::optional<std::string> alarm_phase{};
  std::string search{};
  std::optional<std::string> clock_owner{};
  /// Restrict to one failure capture's protected sequence range.
  std::optional<std::uint64_t> capture_id{};
  /// One application start's rows (16 lowercase hex). Unset, a filter whose
  /// values restart at every start (run, operation, node attempt, Stop,
  /// action span) means the store's own start; other filters span all starts.
  std::optional<std::string> application_start_id{};
};

struct HistorianPage final {
  std::vector<HistorianStoredEvent> events;
  bool truncated;
  std::optional<std::uint64_t> next_before_sequence;
  manyforge::content_identity::CanonicalValue retention{};
  /// The capture's evidence statement when the query selected one capture.
  manyforge::content_identity::CanonicalValue capture{};
};

/// Bounds of failure capture: protection of already-durable rows around an
/// ERROR/CRITICAL event, never a copied dump. Values are measured hypotheses;
/// the combined protection budget stays inside the page-image cap.
struct HistorianCaptureLimits final {
  std::size_t preceding_rows{1000U};
  std::int64_t preceding_bytes{2LL * 1024LL * 1024LL};
  std::size_t tail_rows{200U};
  std::chrono::milliseconds tail_duration{30000};
  std::int64_t capture_bytes{4LL * 1024LL * 1024LL};
  std::size_t live_captures{64U};
  std::int64_t protected_bytes{32LL * 1024LL * 1024LL};
  std::size_t recorded_triggers{32U};
};

/// Monotonic nanoseconds of the writer process. Capture tails are timed only
/// on this clock; it is never compared across processes.
using HistorianWriterClock = std::function<std::int64_t()>;

[[nodiscard]] manyforge::content_identity::CanonicalValue
HistorianPageValue(const HistorianPage &page);
/// Bounded list projection. Full event payloads remain available only through
/// HistorianDetailValue so a History list cannot make the Python control loop
/// parse and discard large nested records.
[[nodiscard]] manyforge::content_identity::CanonicalValue
HistorianSummaryPageValue(const HistorianPage &page);
/// Bounded display text from an operation snapshot's original participant
/// errors. The producer stores it as message; retained older events are
/// unchanged.
[[nodiscard]] std::string OperationFailureHistoryMessage(
    const manyforge::content_identity::CanonicalValue &operation_state);
/// Compact projection of an already validated original. Storage binds its
/// exact bytes to the original digest; list reads never decode full payloads.
[[nodiscard]] manyforge::content_identity::CanonicalValue HistorianSummaryEventValue(
    const HistorianEvent& event, std::optional<std::int64_t> occurred_at_utc_ns,
    const std::optional<std::string>& clock_owner, bool wall_clock_regressed);
[[nodiscard]] manyforge::content_identity::CanonicalValue HistorianSummaryEnvelopeValue(
    manyforge::content_identity::CanonicalValue::Array events, const HistorianPage& page);
[[nodiscard]] manyforge::content_identity::CanonicalValue
HistorianExportPageValue(const HistorianPage &page);
[[nodiscard]] manyforge::content_identity::CanonicalValue
HistorianDetailValue(const HistorianStoredEvent &stored);

class IEventHistoryStore {
public:
  enum class AppendStatus {
    kStored,
    kDuplicate,
  };

  /// Result of a batch append: one status per committed leading event.
  /// When ``failure`` is set, ``events[statuses.size()]`` failed and was not
  /// committed; no later event of the batch was attempted. Without a failure,
  /// fewer statuses than events means the store's transaction bound deferred
  /// the rest (at least one event is always committed).
  struct AppendBatchResult {
    std::vector<AppendStatus> statuses;
    std::exception_ptr failure;
  };

  virtual ~IEventHistoryStore() = default;
  /// Cumulative events this writer could not deliver (queue overflow, store
  /// failures and events that could not be made into a History entry).
  /// Dropped events have no sequence; a store may only use this to mark
  /// coverage uncertain, never to place a loss exactly.
  virtual void ObserveDeliveryLoss(std::uint64_t cumulative_losses) {
    static_cast<void>(cumulative_losses);
  }
  [[nodiscard]] virtual AppendStatus Append(const HistorianEvent &event) = 0;
  /// Append events in order. The default appends them one by one; a store
  /// may commit them together. A failure of the whole write may also be
  /// thrown, in which case nothing of the batch was committed.
  [[nodiscard]] virtual AppendBatchResult
  AppendBatch(std::span<const HistorianEvent> events);
  [[nodiscard]] virtual HistorianPage Query(const HistorianQuery &query) = 0;
  [[nodiscard]] virtual std::optional<HistorianStoredEvent>
  Read(std::uint64_t sequence) = 0;
};

/// History store in a directory. Its file is named by its format version,
/// ``history.v<N>.sqlite3``, and only that file is opened or created; a file
/// holding another format is refused (``historian_storage_foreign``). Earlier
/// History files in the directory are listed once at open, never opened, and
/// reported in the retention document.
class SqliteEventHistoryStore final : public IEventHistoryStore {
public:
  /// The current-format History file in ``directory``.
  [[nodiscard]] static std::filesystem::path DatabasePath(
      const std::filesystem::path &directory);

  /// ``application_start_id`` is the start whose Headless writes through
  /// this store (16 hex). Every appended row records it (the previous-session
  /// report and the alarm rows that embed it record the start the report
  /// describes), and a filter by an id minted inside a start (Program run,
  /// operation, node attempt, Stop, action span) that names no start matches
  /// only rows of this start.
  SqliteEventHistoryStore(
      const std::filesystem::path &directory, std::string application_start_id,
      std::size_t record_retention = kDefaultHistorianRecordRetention,
      HistorianCaptureLimits capture_limits = {},
      HistorianWriterClock writer_clock = {});
  ~SqliteEventHistoryStore() override;

  SqliteEventHistoryStore(const SqliteEventHistoryStore &) = delete;
  SqliteEventHistoryStore &operator=(const SqliteEventHistoryStore &) = delete;

  void ObserveDeliveryLoss(std::uint64_t cumulative_losses) override;
  [[nodiscard]] AppendStatus Append(const HistorianEvent &event) override;
  /// One transaction for the whole batch, one savepoint per event.
  [[nodiscard]] AppendBatchResult
  AppendBatch(std::span<const HistorianEvent> events) override;
  [[nodiscard]] HistorianPage Query(const HistorianQuery &query) override;
  /// Bounded newest-first list of failure captures (at most 32 per page).
  [[nodiscard]] manyforge::content_identity::CanonicalValue Captures(
      std::optional<std::uint64_t> before_capture_id, std::size_t limit);
  [[nodiscard]] manyforge::content_identity::CanonicalValue QuerySummaries(
      const HistorianQuery& query);
  /// Apply the export byte budget before decoding full retained records.
  [[nodiscard]] HistorianPage QueryExport(const HistorianQuery& query);
  /// A start-scoped kind (``program_run_id`` and the other ids minted
  /// inside a start) lists one start's values (``application_start_id``,
  /// else this store's start) in number order; other kinds span all starts.
  [[nodiscard]] manyforge::content_identity::CanonicalValue Discover(
      const std::string& kind, const std::string& after, std::size_t limit,
      const std::optional<std::string>& application_start_id = std::nullopt);
  [[nodiscard]] std::optional<HistorianStoredEvent>
  Read(std::uint64_t sequence) override;
  void Close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// History's writer health. A record History cannot take or write is dropped
/// and counted; nothing waits for it and nothing else is gated by it. (A full
/// queue or a full disk stops operation through the disk check, OD-215,
/// which reads `AsyncEventHistorian::DiskSignals`, not this health.)
struct AsyncHistorianHealth final {
  std::string schema_version;
  /// False during a loss episode: from the first dropped record until a
  /// successful write leaves the queue empty again.
  bool available;
  bool accepting;
  /// Advances whenever the loss cause or the episode changes.
  std::uint64_t health_generation;
  /// The latest loss cause of the current episode; empty outside one.
  std::optional<std::string> error_code;
  std::optional<std::string> error_message;
  std::size_t queue_depth;
  std::size_t queue_capacity;
  bool write_in_flight;
  std::uint64_t accepted_count;
  std::uint64_t delivered_count;
  std::uint64_t duplicate_count;
  /// Records dropped because the queue was full.
  std::uint64_t overflow_count;
  /// Records dropped because they could not be made into a History entry or
  /// their write failed.
  std::uint64_t delivery_failure_count;
};

[[nodiscard]] manyforge::content_identity::CanonicalValue
AsyncHistorianHealthValue(const AsyncHistorianHealth &health);

/// What the Headless disk check (OD-215) reads from History's writer, under
/// the writer's own lock only.
struct AsyncHistorianDiskSignals final {
  /// Set when a record is dropped because the queue is full; cleared when a
  /// batch, written or not, leaves the queue empty.
  bool queue_full;
  /// The current loss episode's latest cause is a write that found the disk
  /// full (`historian_storage_full`).
  bool storage_full;
};

class AsyncEventHistorian final {
public:
  explicit AsyncEventHistorian(
      std::shared_ptr<IEventHistoryStore> store,
      std::size_t queue_capacity = kDefaultHistorianQueueCapacity,
      std::chrono::milliseconds commit_gap = kDefaultHistorianCommitGap);
  ~AsyncEventHistorian();

  AsyncEventHistorian(const AsyncEventHistorian &) = delete;
  AsyncEventHistorian &operator=(const AsyncEventHistorian &) = delete;

  /// Non-blocking with respect to durable storage. Returns false when the
  /// bounded in-memory delivery queue cannot accept the event: it is dropped
  /// and counted.
  [[nodiscard]] bool TryEnqueue(HistorianEvent event);
  /// A committed fact could not be made into a History entry (for example
  /// during encoding): it is counted as dropped like any other lost record.
  void ReportDeliveryFailure(std::string code, std::string message);
  [[nodiscard]] AsyncHistorianHealth Health() const;
  /// The disk check's reading; takes only this writer's lock (memory only).
  [[nodiscard]] AsyncHistorianDiskSignals DiskSignals() const;
  [[nodiscard]] bool WaitUntilIdle(std::chrono::milliseconds timeout) const;
  void Shutdown();

private:
  void Run();
  /// Starts or continues the loss episode with this cause. The writer logs
  /// one line per episode, outside the lock.
  void MarkLossLocked(std::string code, std::string message);

  std::shared_ptr<IEventHistoryStore> store_;
  std::size_t queue_capacity_;
  std::chrono::milliseconds commit_gap_;
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  mutable std::condition_variable idle_condition_;
  std::deque<HistorianEvent> queue_;
  /// Identities of the queued events: the duplicate check is one lookup.
  std::unordered_set<std::string> queued_ids_;
  std::optional<std::chrono::steady_clock::time_point> last_commit_;
  bool accepting_{true};
  bool stop_requested_{};
  bool write_in_flight_{};
  /// A loss episode: from the first dropped record until a successful write
  /// leaves the queue empty.
  bool loss_active_{};
  /// OD-215: a record was dropped because the queue was full, and no batch
  /// has left the queue empty since.
  bool queue_full_{};
  /// The episode's one log line, until the writer writes it.
  std::optional<std::string> unlogged_loss_;
  std::uint64_t health_generation_{};
  std::optional<std::string> error_code_;
  std::optional<std::string> error_message_;
  std::uint64_t accepted_count_{};
  std::uint64_t delivered_count_{};
  std::uint64_t duplicate_count_{};
  std::uint64_t overflow_count_{};
  std::uint64_t delivery_failure_count_{};
  std::thread worker_;
};

} // namespace manyforge::headless
