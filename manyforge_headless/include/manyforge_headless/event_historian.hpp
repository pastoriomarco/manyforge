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

#include <manyforge_headless/operator_alarm_owner.hpp>

namespace manyforge::headless {

inline constexpr std::string_view kHistorianEventSchemaVersion =
    "manyforge.historian_event.v2";
inline constexpr std::string_view kHistorianPageSchemaVersion =
    "manyforge.historian_page.v2";
inline constexpr std::string_view kHistorianSummaryPageSchemaVersion =
    "manyforge.historian_summary_page.v2";
inline constexpr std::string_view kHistorianEventSummarySchemaVersion =
    "manyforge.historian_event_summary.v2";
inline constexpr std::string_view kHistorianDetailSchemaVersion =
    "manyforge.historian_detail.v2";
inline constexpr std::string_view kHistorianCapturePageSchemaVersion =
    "manyforge.historian_capture_page.v1";
/// Retained Program diagnostic versions. Only their producer-level identity
/// fields are correlation attribution; blackboard/override values are data.
/// Ingress accepts only the current version; originals recorded before a
/// schema cutover keep their recorded version and this derivation.
inline constexpr std::array<std::string_view, 3>
    kProgramDiagnosticSchemaVersions{"manyforge.program_diagnostic.v1",
                                     "manyforge.program_diagnostic.v2",
                                     "manyforge.program_diagnostic.v3"};
inline constexpr std::size_t kDefaultHistorianQueueCapacity = 1024U;
/// Minimum interval between the starts of two writer commits. Events that
/// arrive meanwhile wait in memory and share the next transaction.
inline constexpr std::chrono::milliseconds kDefaultHistorianCommitGap{50};
/// Upper bound of events in one writer transaction.
inline constexpr std::size_t kMaximumHistorianBatchEvents = 128U;
inline constexpr std::size_t kDefaultHistorianRecordRetention = 100000U;
inline constexpr std::size_t kMaximumHistorianPageSize = 10000U;
inline constexpr std::size_t kMaximumHistorianPayloadBytes = 256U * 1024U;

class EventHistorianError : public std::runtime_error {
public:
  EventHistorianError(std::string code, std::string message);
  [[nodiscard]] const std::string &code() const noexcept;

private:
  std::string code_;
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
  /// Cumulative events this writer could not deliver (queue overflow and
  /// store failures). Dropped events have no sequence; a store may only use
  /// this to mark coverage uncertain, never to place a loss exactly.
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

class SqliteEventHistoryStore final : public IEventHistoryStore {
public:
  explicit SqliteEventHistoryStore(
      std::filesystem::path database_path,
      std::size_t record_retention = kDefaultHistorianRecordRetention,
      HistorianCaptureLimits capture_limits = {},
      HistorianWriterClock writer_clock = {});
  ~SqliteEventHistoryStore() override;

  SqliteEventHistoryStore(const SqliteEventHistoryStore &) = delete;
  SqliteEventHistoryStore &operator=(const SqliteEventHistoryStore &) = delete;

  void ObserveDeliveryLoss(std::uint64_t cumulative_losses) override;
  [[nodiscard]] AppendStatus Append(const HistorianEvent &event) override;
  /// One transaction for the whole batch, one savepoint per event. A batch
  /// holding an ERROR/CRITICAL condition or a previous-session report is also synced to disk.
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
  [[nodiscard]] manyforge::content_identity::CanonicalValue Discover(const std::string& kind,
                                                                     const std::string& after,
                                                                     std::size_t limit);
  [[nodiscard]] std::optional<HistorianStoredEvent>
  Read(std::uint64_t sequence) override;
  void Close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

struct AsyncHistorianHealth final {
  std::string schema_version;
  bool available;
  bool recovery_ready;
  bool accepting;
  std::uint64_t health_generation;
  std::optional<std::string> error_code;
  std::optional<std::string> error_message;
  std::size_t queue_depth;
  std::size_t queue_capacity;
  bool write_in_flight;
  std::uint64_t accepted_count;
  std::uint64_t delivered_count;
  std::uint64_t duplicate_count;
  std::uint64_t overflow_count;
  std::uint64_t delivery_failure_count;
};

[[nodiscard]] manyforge::content_identity::CanonicalValue
AsyncHistorianHealthValue(const AsyncHistorianHealth &health);

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
  /// bounded in-memory delivery queue cannot accept the event.
  [[nodiscard]] bool TryEnqueue(HistorianEvent event);
  /// A committed fact failed before enqueue (for example during encoding).
  /// Shares the same generation-fenced health recovery as store/queue failures.
  void ReportDeliveryFailure(std::string code, std::string message);
  [[nodiscard]] AsyncHistorianHealth Health() const;
  [[nodiscard]] bool WaitUntilIdle(std::chrono::milliseconds timeout) const;

  /// Clears one observed failure episode only after at least one subsequent
  /// durable write. The exact generation prevents stale health repair.
  [[nodiscard]] bool ReconcileHealth(std::uint64_t expected_generation);
  void Shutdown();

private:
  friend struct AsyncEventHistorianTestAccess;
  void Run();
  void MarkFailureLocked(std::string code, std::string message);
  [[nodiscard]] bool AdvanceHealthGenerationLocked();

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
  bool failure_active_{};
  bool recovery_write_observed_{};
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
