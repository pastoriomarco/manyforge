#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>

#include <manyforge_headless/operator_alarm_owner.hpp>

namespace manyforge::headless {

inline constexpr std::string_view kHistorianEventSchemaVersion =
    "manyforge.historian_event.v2";
inline constexpr std::string_view kHistorianPageSchemaVersion =
    "manyforge.historian_page.v2";
inline constexpr std::string_view kHistorianSummaryPageSchemaVersion =
    "manyforge.historian_summary_page.v1";
inline constexpr std::string_view kHistorianEventSummarySchemaVersion =
    "manyforge.historian_event_summary.v1";
inline constexpr std::string_view kHistorianDetailSchemaVersion =
    "manyforge.historian_detail.v2";
inline constexpr std::size_t kDefaultHistorianQueueCapacity = 1024U;
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
};

struct HistorianQuery final {
  std::optional<std::int64_t> start_ns;
  std::optional<std::int64_t> end_ns;
  std::set<std::string> components;
  std::set<std::string> event_types;
  std::optional<std::uint64_t> before_sequence;
  std::size_t limit{1000U};
};

struct HistorianPage final {
  std::vector<HistorianStoredEvent> events;
  bool truncated;
  std::optional<std::uint64_t> next_before_sequence;
};

[[nodiscard]] manyforge::content_identity::CanonicalValue
HistorianPageValue(const HistorianPage &page);
/// Bounded list projection. Full event payloads remain available only through
/// HistorianDetailValue so a History list cannot make the Python control loop
/// parse and discard large nested records.
[[nodiscard]] manyforge::content_identity::CanonicalValue
HistorianSummaryPageValue(const HistorianPage &page);
[[nodiscard]] manyforge::content_identity::CanonicalValue
HistorianDetailValue(const HistorianStoredEvent &stored);

class IEventHistoryStore {
public:
  enum class AppendStatus {
    kStored,
    kDuplicate,
  };

  virtual ~IEventHistoryStore() = default;
  [[nodiscard]] virtual AppendStatus Append(const HistorianEvent &event) = 0;
  [[nodiscard]] virtual HistorianPage Query(const HistorianQuery &query) = 0;
  [[nodiscard]] virtual std::optional<HistorianStoredEvent>
  Read(std::uint64_t sequence) = 0;
};

class SqliteEventHistoryStore final : public IEventHistoryStore {
public:
  explicit SqliteEventHistoryStore(
      std::filesystem::path database_path,
      std::size_t record_retention = kDefaultHistorianRecordRetention);
  ~SqliteEventHistoryStore() override;

  SqliteEventHistoryStore(const SqliteEventHistoryStore &) = delete;
  SqliteEventHistoryStore &operator=(const SqliteEventHistoryStore &) = delete;

  [[nodiscard]] AppendStatus Append(const HistorianEvent &event) override;
  [[nodiscard]] HistorianPage Query(const HistorianQuery &query) override;
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
      std::size_t queue_capacity = kDefaultHistorianQueueCapacity);
  ~AsyncEventHistorian();

  AsyncEventHistorian(const AsyncEventHistorian &) = delete;
  AsyncEventHistorian &operator=(const AsyncEventHistorian &) = delete;

  /// Non-blocking with respect to durable storage. Returns false when the
  /// bounded in-memory delivery queue cannot accept the event.
  [[nodiscard]] bool TryEnqueue(HistorianEvent event);
  [[nodiscard]] AsyncHistorianHealth Health() const;
  [[nodiscard]] bool WaitUntilIdle(std::chrono::milliseconds timeout) const;

  /// Clears one observed failure episode only after at least one subsequent
  /// durable write. The exact generation prevents stale health repair.
  [[nodiscard]] bool ReconcileHealth(std::uint64_t expected_generation);
  void Shutdown();

private:
  void Run();
  void MarkFailureLocked(std::string code, std::string message);

  std::shared_ptr<IEventHistoryStore> store_;
  std::size_t queue_capacity_;
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  mutable std::condition_variable idle_condition_;
  std::deque<HistorianEvent> queue_;
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
