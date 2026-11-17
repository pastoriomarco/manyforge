#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <exception>
#include <span>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

#include "manyforge_headless/event_historian.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;
using namespace std::chrono_literals;

/// The application start whose Headless writes through the test stores.
const std::string kStart = "0123456789abcdef";

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-event-historian-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("historian test mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

// A History directory of its own below ``directory``; the store names its file.
std::filesystem::path HistoryDirectory(const TemporaryDirectory &directory, const std::string &name) {
  const auto path = directory.path() / name;
  std::filesystem::create_directory(path);
  return path;
}

HistorianEvent Event(const std::uint64_t index,
                     std::string component = "manyforge.alarm",
                     std::string event_type = "operator_alarm_activated") {
  return MakeHistorianEvent(
      std::move(component), std::move(event_type), "INFO",
      static_cast<std::int64_t>(100U + index), "2026-08-05T00:00:00Z",
      CanonicalValue(CanonicalValue::Object{
          {"index", CanonicalValue(static_cast<double>(index))},
      }));
}

HistorianEvent OperationFailureEvent(const std::string &message) {
  auto event = MakeHistorianEvent(
      "manyforge.runtime", "motion_retry_selected", "ERROR", 101,
      "2026-08-05T00:00:00Z",
      CanonicalValue(CanonicalValue::Object{
          {"schema_version",
           CanonicalValue("manyforge.operation_history_event.v2")},
          {"application_start_id", CanonicalValue(kStart)},
          {"operation_id", CanonicalValue(1.0)},
          {"request_key", CanonicalValue("c0000000000000050000000000000001")},
          {"journal_payload",
           CanonicalValue(CanonicalValue::Object{
               {"operation_state",
                CanonicalValue(CanonicalValue::Object{
                    {"operation_id", CanonicalValue(1.0)},
                    {"program_run_id", CanonicalValue(1.0)},
                    {"participants",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue(CanonicalValue::Object{
                             {"execution_scope",
                              CanonicalValue(CanonicalValue::Object{
                                  {"node_instance_id", CanonicalValue("move")},
                                  {"node_attempt_id",
                                   CanonicalValue(1.0)}})},
                             {"terminal_error",
                              CanonicalValue(CanonicalValue::Object{
                                  {"code", CanonicalValue("planning_failed")},
                                  {"message",
                                   CanonicalValue(message)}})}})})}})}})}}));
  const auto &state = *event.payload.find_object_member("journal_payload")
                           ->find_object_member("operation_state");
  auto payload = event.payload.as_object();
  payload.emplace_back("message",
                       CanonicalValue(OperationFailureHistoryMessage(state)));
  return MakeHistorianEvent(event.component, event.event_type, event.severity,
                            event.occurred_at_ns, event.occurred_at_wall,
                            CanonicalValue(std::move(payload)));
}

// Partition-retention tests isolate ordinary pruning: each error capture
// protects only its trigger, closes at once and retires the previous one.
HistorianCaptureLimits TriggerOnlyCaptureLimits() {
  HistorianCaptureLimits limits;
  limits.preceding_rows = 0U;
  limits.tail_rows = 0U;
  limits.live_captures = 1U;
  return limits;
}

// The recorded CCLC component incident row (`manyforge.cclc`), as on the
// live Workbench: codes sit in `details`, the summary is not search text.
HistorianEvent ComponentIncidentEvent(std::int64_t index, const std::string &cause, bool closed) {
  const auto incident_id = "incident.arm_robot." + std::to_string(index);
  const CanonicalValue codes(CanonicalValue::Array{CanonicalValue("C2"), CanonicalValue("mode=0"),
                                                   CanonicalValue("state=" + std::to_string(index))});
  CanonicalValue::Object details{
      {"cause_class", CanonicalValue(cause)},
      {"controllable_component_id", CanonicalValue("arm_robot")},
      {"event_kind", CanonicalValue(closed ? "component_incident_acknowledged" : "component_incident")},
      {"incident_id", CanonicalValue(incident_id)},
      {"providers", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                        {"provider_instance_id", CanonicalValue("arm_robot.providerneedle")}})})},
      {"schema_version", CanonicalValue("manyforge.component_incident_event.v2")},
      {"summary", CanonicalValue("Component arm_robot summaryneedle " + cause + ".")},
      {"timeline", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                       {"cause_class", CanonicalValue(cause)}, {"vendor_codes", codes}})})},
      {"vendor_codes", codes}};
  if (closed) details.emplace_back("closure_reason", CanonicalValue("cell_closed_by_operator"));
  return MakeHistorianEvent(
      "manyforge.cclc", closed ? "component_incident_acknowledged" : "component_incident", closed ? "INFO" : "ERROR",
      index, "2026-10-28T00:00:00Z",
      CanonicalValue(CanonicalValue::Object{
          {"cclc_owner_epoch", CanonicalValue("sha256:epochneedle")},
          {"details", CanonicalValue(std::move(details))},
          {"event_kind", CanonicalValue(closed ? "component_incident_acknowledged" : "component_incident")},
          {"incident_id", CanonicalValue(incident_id)},
          {"schema_version", CanonicalValue("manyforge.cclc_incident_history_event.v2")}}));
}

template <typename Callback>
void ExpectHistorianError(Callback &&callback,
                          const std::string_view expected_code) {
  try {
    callback();
    FAIL() << "expected EventHistorianError " << expected_code;
  } catch (const EventHistorianError &error) {
    EXPECT_EQ(error.code(), expected_code);
  }
}

// Capture the next SQLite connection on this test thread, so a test can set
// a small page limit without adding a production-only configuration hook.
thread_local sqlite3 *captured_test_database = nullptr;

int CaptureTestDatabase(sqlite3 *database, char **, const sqlite3_api_routines *) {
  captured_test_database = database;
  return SQLITE_OK;
}

class ScopedTestDatabaseCapture final {
public:
  ScopedTestDatabaseCapture()
      : entry_(reinterpret_cast<void (*)()>(&CaptureTestDatabase)),
        result_(sqlite3_auto_extension(entry_)) {
    captured_test_database = nullptr;
  }
  ~ScopedTestDatabaseCapture() {
    if (result_ == SQLITE_OK) sqlite3_cancel_auto_extension(entry_);
    captured_test_database = nullptr;
  }
  [[nodiscard]] int result() const { return result_; }
private:
  void (*entry_)();
  int result_;
};

class ControllableStore final : public IEventHistoryStore {
public:
  AppendStatus Append(const HistorianEvent &event) override {
    std::unique_lock lock(mutex_);
    ++append_calls_;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return !blocked_; });
    if (failing_ || failing_ids_.contains(event.event_id)) {
      throw EventHistorianError(failure_code_,
                                "injected historian failure");
    }
    const auto encoded =
        content_identity::canonical_json(HistorianEventValue(event));
    const auto found = events_.find(event.event_id);
    if (found != events_.end()) {
      if (found->second != encoded) {
        throw EventHistorianError("historian_event_identity_conflict",
                                  "injected identity conflict");
      }
      return AppendStatus::kDuplicate;
    }
    events_.insert_or_assign(event.event_id, encoded);
    return AppendStatus::kStored;
  }

  AppendBatchResult AppendBatch(std::span<const HistorianEvent> events) override {
    {
      std::lock_guard lock(mutex_);
      batch_sizes_.push_back(events.size());
      if (batch_throws_) {
        // A failure of the whole write: nothing of the batch is committed.
        throw EventHistorianError("historian_storage_busy", "injected write failure");
      }
    }
    return IEventHistoryStore::AppendBatch(events);
  }

  HistorianPage Query(const HistorianQuery &) override {
    return {{}, false, std::nullopt};
  }

  std::optional<HistorianStoredEvent> Read(std::uint64_t) override {
    return std::nullopt;
  }

  void SetBlocked(const bool blocked) {
    std::lock_guard lock(mutex_);
    blocked_ = blocked;
    condition_.notify_all();
  }

  void SetFailing(const bool failing, std::string code = "injected_storage_failure") {
    std::lock_guard lock(mutex_);
    failing_ = failing;
    failure_code_ = std::move(code);
    condition_.notify_all();
  }

  /// Only this record fails, every time it is written.
  void FailEvent(std::string event_id) {
    std::lock_guard lock(mutex_);
    failing_ids_.insert(std::move(event_id));
  }

  void SetBatchThrows(const bool throws) {
    std::lock_guard lock(mutex_);
    batch_throws_ = throws;
  }

  [[nodiscard]] bool Stored(const std::string &event_id) const {
    std::lock_guard lock(mutex_);
    return events_.contains(event_id);
  }

  bool WaitForAppendCalls(const std::size_t expected,
                          const std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this, expected]() {
      return append_calls_ >= expected;
    });
  }

  [[nodiscard]] std::size_t StoredCount() const {
    std::lock_guard lock(mutex_);
    return events_.size();
  }

  [[nodiscard]] std::vector<std::size_t> BatchSizes() const {
    std::lock_guard lock(mutex_);
    return batch_sizes_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool blocked_{};
  bool failing_{};
  bool batch_throws_{};
  std::set<std::string> failing_ids_;
  std::string failure_code_{"injected_storage_failure"};
  std::size_t append_calls_{};
  std::vector<std::size_t> batch_sizes_;
  std::map<std::string, std::string> events_;
};

TEST(EventHistorian, SqliteStoreIsIdempotentRetainedRestartableAndPaged) {
  TemporaryDirectory directory;
  {
    SqliteEventHistoryStore store(directory.path(), kStart, 3U);
    for (std::uint64_t index = 1U; index <= 5U; ++index) {
      const auto status = store.Append(
          Event(index, index % 2U == 0U ? "manyforge.scene" : "manyforge.alarm",
                index == 4U ? "operator_alarm_returned"
                            : "operator_alarm_activated"));
      EXPECT_EQ(status, IEventHistoryStore::AppendStatus::kStored);
    }
    EXPECT_EQ(store.Append(Event(5U)),
              IEventHistoryStore::AppendStatus::kDuplicate);

    auto page = store.Query(
        HistorianQuery{std::nullopt, std::nullopt, {}, {}, std::nullopt, 2U});
    ASSERT_EQ(page.events.size(), 2U);
    EXPECT_TRUE(page.truncated);
    EXPECT_EQ(
        std::get<double>(
            page.events[0].event.payload.as_object().front().second.storage()),
        5.0);
    EXPECT_EQ(
        std::get<double>(
            page.events[1].event.payload.as_object().front().second.storage()),
        4.0);
    ASSERT_TRUE(page.next_before_sequence.has_value());

    const auto next = store.Query(HistorianQuery{
        std::nullopt, std::nullopt, {}, {}, page.next_before_sequence, 2U});
    ASSERT_EQ(next.events.size(), 1U);
    EXPECT_FALSE(next.truncated);
    EXPECT_EQ(std::get<double>(next.events.front()
                                   .event.payload.as_object()
                                   .front()
                                   .second.storage()),
              3.0);

    const auto filtered = store.Query(HistorianQuery{
        std::nullopt,
        std::nullopt,
        {"manyforge.scene"},
        {"operator_alarm_returned"},
        std::nullopt,
        10U,
    });
    ASSERT_EQ(filtered.events.size(), 1U);
    EXPECT_EQ(filtered.events.front().event.event_type,
              "operator_alarm_returned");

    const auto exact = store.Read(4U);
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ(exact->sequence, 4U);
    EXPECT_EQ(exact->event.event_id,
              Event(4U, "manyforge.scene", "operator_alarm_returned").event_id);
    EXPECT_FALSE(store.Read(2U).has_value());
  }

  SqliteEventHistoryStore reopened(directory.path(), kStart, 3U);
  const auto page = reopened.Query(HistorianQuery{});
  ASSERT_EQ(page.events.size(), 3U);
  EXPECT_EQ(page.events.front().event.event_id, Event(5U).event_id);
}

TEST(EventHistorian, RecordedUtcIsExactAndInvalidHistoryHasNoInventedTime) {
  EXPECT_EQ(HistorianRecordedUtcNs("1970-01-01T00:00:00Z"), 0);
  EXPECT_EQ(HistorianRecordedUtcNs("1970-01-01T01:00:00.000000001+01:00"), 1);
  EXPECT_EQ(HistorianRecordedUtcNs("2262-04-11T23:47:16.854775807Z"), std::numeric_limits<std::int64_t>::max());
  for (const auto bad : {"unknown", "2026-02-30T00:00:00Z", "2026-09-06T12:00:00",
                         "2026-09-06T12:00:60Z", "2262-04-11T23:47:16.854775808Z",
                         "1970-01-01T00:00:00.0000000001Z", "1969-12-31T23:59:59Z"}) {
    SCOPED_TRACE(bad);
    EXPECT_FALSE(HistorianRecordedUtcNs(bad));
  }
}

TEST(EventHistorian, IndexedFiltersFindOlderErrorsAcrossClockDomainsBeforePagination) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart, 128U);
  const auto payload = [](std::string clock, std::string message) {
    return CanonicalValue(CanonicalValue::Object{
        {"clock_owner", CanonicalValue(std::move(clock))}, {"message", CanonicalValue(std::move(message))},
        {"detail", CanonicalValue(CanonicalValue::Object{
          {"program_run_id", CanonicalValue(7.0)}, {"action_span_id", CanonicalValue("action.needle")},
          {"branches", CanonicalValue(CanonicalValue::Array{
            CanonicalValue(CanonicalValue::Object{{"operation_id", CanonicalValue(11.0)}}),
            CanonicalValue(CanonicalValue::Object{{"operation_id", CanonicalValue(12.0)}})})}})},
    });
  };
  EXPECT_EQ(store.Append(MakeHistorianEvent("manyforge.kernel", "operation_failed", "ERROR",
      9007199254740992LL, "2026-09-06T12:00:00.000000001Z", payload("kernel.1", "failed quoted \"gripper\" pose"))),
      IEventHistoryStore::AppendStatus::kStored);
  EXPECT_EQ(store.Append(MakeHistorianEvent("manyforge.composer", "client_incident_activated", "ERROR",
      1, "2026-09-06T12:00:00.000000002Z", payload("headless.2", "Control connection became unavailable"))),
      IEventHistoryStore::AppendStatus::kStored);
  for (std::uint64_t index = 1; index <= 40; ++index) EXPECT_EQ(store.Append(Event(index)), IEventHistoryStore::AppendStatus::kStored);
  HistorianQuery query;
  query.limit = 1U;
  query.start_utc_ns = HistorianRecordedUtcNs("2026-09-06T12:00:00.000000001Z");
  query.end_utc_ns = HistorianRecordedUtcNs("2026-09-06T12:00:00.000000002Z");
  query.severities = {"ERROR"};
  query.correlations = {{"program_run_id", "7"}, {"operation_id", "12"}, {"action_span_id", "action.needle"}};
  auto page = store.Query(query);
  ASSERT_EQ(page.events.size(), 1U);
  EXPECT_EQ(page.events[0].event.component, "manyforge.composer");
  EXPECT_TRUE(page.truncated);
  query.before_sequence = page.next_before_sequence;
  page = store.Query(query);
  ASSERT_EQ(page.events.size(), 1U);
  EXPECT_EQ(page.events[0].event.component, "manyforge.kernel");
  EXPECT_FALSE(page.truncated);
  query.before_sequence.reset();
  query.search = "\"gripper\"";
  EXPECT_EQ(store.Query(query).events.size(), 1U);
  query.search = "CONNECTION BECAME";
  EXPECT_EQ(store.Query(query).events.size(), 1U);
  query.search = "unmatched";
  EXPECT_TRUE(store.Query(query).events.empty());
  query.search = "ab";
  ExpectHistorianError([&] { (void)store.Query(query); }, "historian_query_invalid");
  query.search.clear();
  query.start_ns = 0;
  ExpectHistorianError([&] { (void)store.Query(query); }, "historian_query_invalid");
  query.clock_owner = "headless.2";
  EXPECT_EQ(store.Query(query).events.size(), 1U);
}

TEST(EventHistorian, ProgramDiagnosticValuesAreNeitherSearchTextNorOwnerIdentities) {
  TemporaryDirectory directory;
  // Originals recorded before the v3 cutover keep their version and derivation.
  for (const auto version : kProgramDiagnosticSchemaVersions) {
    const auto history = HistoryDirectory(directory, std::string(version));
    CanonicalValue::Object payload{
        {"schema_version", CanonicalValue(std::string(version))},
        {"program_run_id", CanonicalValue(123.0)},
        {"blackboard", CanonicalValue(CanonicalValue::Object{
                           {"operation_id", CanonicalValue(789.0)},
                           {"program_run_id", CanonicalValue(456.0)}})},
    };
    if (version == "manyforge.program_diagnostic.v1" ||
        version == "manyforge.program_diagnostic.v2") {
      // Retained v1/v2 rows also carry the withdrawn runtime-override section.
      payload.emplace_back("runtime_overrides",
                           CanonicalValue(CanonicalValue::Object{
                               {"operation_id", CanonicalValue(789.0)}}));
    }
    const auto event = MakeHistorianEvent(
        "manyforge.program_runner", "program_failed", "ERROR", 100, "2026-09-08T00:00:00Z",
        CanonicalValue(std::move(payload)));
    for (const bool reopened : {false, true}) {
      SqliteEventHistoryStore store(history, kStart, 128U);
      if (!reopened) {
        EXPECT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
      }
      HistorianQuery query;
      query.correlations = {{"program_run_id", "123"}};
      ASSERT_EQ(store.Query(query).events.size(), 1U);
      const auto summary_page = store.QuerySummaries(query);
      const auto& summary = *summary_page.find_object_member("events")
                                 ->as_array()
                                 .front()
                                 .find_object_member("event");
      EXPECT_EQ(summary.find_object_member("program_run_id")->as_string(), "123");
      EXPECT_EQ(content_identity::canonical_json(*summary.find_object_member("operation_id")),
                "null");
      query.correlations = {{"program_run_id", "456"}};
      EXPECT_TRUE(store.Query(query).events.empty());
      query.correlations = {{"operation_id", "789"}};
      EXPECT_TRUE(store.Query(query).events.empty());
      query.correlations.clear();
      // Search covers the producer identities, not user values in the payload.
      query.search = "456";
      EXPECT_TRUE(store.Query(query).events.empty());
      query.search = "123";
      EXPECT_EQ(store.Query(query).events.size(), 1U);
    }
  }
}

TEST(EventHistorian, ClockRegressionsAreNotConfusedWithDelayedOtherOwnersOrOldEvents) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart);
  const auto append = [&](std::int64_t monotonic, std::string wall, std::string clock) {
    return store.Append(MakeHistorianEvent("manyforge.test", "clock_test", "INFO", monotonic, std::move(wall),
        CanonicalValue(CanonicalValue::Object{{"clock_owner", CanonicalValue(std::move(clock))}})));
  };
  (void)append(20, "2026-09-06T12:00:00Z", "owner.1");
  (void)append(21, "2026-09-06T11:59:59Z", "owner.1");
  (void)append(10, "2026-09-06T11:59:50Z", "owner.1"); // old delayed record, not a jump
  (void)append(22, "2026-09-05T11:59:50Z", "owner.2"); // unrelated clock
  EXPECT_TRUE(store.Read(2)->wall_clock_regressed);
  EXPECT_FALSE(store.Read(3)->wall_clock_regressed);
  EXPECT_FALSE(store.Read(4)->wall_clock_regressed);
  (void)append(100, "2026-09-06T12:00:10Z", "owner.late");
  (void)append(200, "2026-09-06T12:00:20Z", "owner.late");
  (void)append(150, "2026-09-06T12:00:05Z", "owner.late");
  EXPECT_TRUE(store.Read(7)->wall_clock_regressed); // exact retained predecessor, not maximum timestamp
  (void)append(175, "2026-09-06T12:00:30Z", "owner.late");
  EXPECT_TRUE(store.Read(6)->wall_clock_regressed); // late predecessor proves successor's uncertainty
}

TEST(EventHistorian, DetailReadsMaximumLegalPrimaryKeyWithoutArithmetic) {
  TemporaryDirectory directory;
  const auto path = SqliteEventHistoryStore::DatabasePath(directory.path());
  {
    SqliteEventHistoryStore store(directory.path(), kStart);
    (void)store.Append(Event(1));
  }
  sqlite3 *raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw, "UPDATE sqlite_sequence SET seq=9007199254740990 WHERE name='historian_events'", nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_close(raw);
  SqliteEventHistoryStore store(directory.path(), kStart);
  (void)store.Append(Event(2));
  const auto last = store.Read(9007199254740991ULL);
  ASSERT_TRUE(last);
  EXPECT_EQ(last->event.event_id, Event(2).event_id);
  EXPECT_FALSE(store.Read(9007199254740990ULL));
  ExpectHistorianError([&] { (void)store.Append(Event(3)); }, "historian_sequence_rollover_required");
}

TEST(EventHistorian, LongServiceNanosecondsRemainExactAndRestartable) {
  const auto legacy = Event(1U);
  const auto legacy_persistence =
      content_identity::canonical_json(HistorianPersistenceValue(legacy));
  EXPECT_NE(legacy_persistence.find("manyforge.historian_event.v1"),
            std::string::npos);
  EXPECT_NE(legacy_persistence.find("\"occurred_at_ns\":101"),
            std::string::npos);
  const auto legacy_public =
      content_identity::canonical_json(HistorianEventValue(legacy));
  EXPECT_NE(legacy_public.find("manyforge.historian_event.v3"),
            std::string::npos);
  EXPECT_NE(legacy_public.find("\"occurred_at_ns\":\"101\""),
            std::string::npos);

  constexpr std::int64_t timestamp = 9007199254740992LL;
  const auto event =
      MakeHistorianEvent("manyforge.composer", "client_incident_resolved",
                         "INFO", timestamp, "2026-09-04T12:00:00Z",
                         CanonicalValue(CanonicalValue::Object{
                             {"incident_id", CanonicalValue("incident.1")},
                         }));
  const auto public_value =
      content_identity::canonical_json(HistorianEventValue(event));
  EXPECT_NE(public_value.find("manyforge.historian_event.v3"),
            std::string::npos);
  EXPECT_NE(public_value.find("\"occurred_at_ns\":\"9007199254740992\""),
            std::string::npos);

  TemporaryDirectory directory;
  {
    SqliteEventHistoryStore store(directory.path(), kStart, 2U);
    EXPECT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
  }
  SqliteEventHistoryStore reopened(directory.path(), kStart, 2U);
  const auto page = reopened.Query(HistorianQuery{});
  ASSERT_EQ(page.events.size(), 1U);
  EXPECT_EQ(page.events.front().event.occurred_at_ns, timestamp);
}

TEST(EventHistorian, ImportantRetentionSurvivesRoutineChurnAndReportsPrunedCoverage) {
  TemporaryDirectory directory;
  const auto important = MakeHistorianEvent("manyforge.composer", "client_incident_resolved", "INFO", 9,
      "2026-09-05T00:00:00Z", CanonicalValue(CanonicalValue::Object{
          {"schema_version", CanonicalValue("manyforge.client_incident_event.v2")},
          {"original_severity", CanonicalValue("ERROR")},
          {"message", CanonicalValue("retained gripper fault")},
      }));
  {
    SqliteEventHistoryStore store(directory.path(), kStart, 2U, TriggerOnlyCaptureLimits());
    ASSERT_EQ(store.Append(important), IEventHistoryStore::AppendStatus::kStored);
    for (std::uint64_t index = 1; index <= 20; ++index) {
      ASSERT_EQ(store.Append(Event(index, "manyforge.runtime", "operation_progressed")), IEventHistoryStore::AppendStatus::kStored);
      EXPECT_EQ(store.Append(important), IEventHistoryStore::AppendStatus::kDuplicate);
    }
    const auto page = store.Query(HistorianQuery{});
    ASSERT_EQ(page.events.size(), 3U);
    EXPECT_EQ(page.events.front().sequence, 21U); // Duplicates allocate no sequence.
    EXPECT_TRUE(store.Read(1U));
    EXPECT_FALSE(store.Read(2U));
    const auto retention = content_identity::canonical_json(page.retention);
    EXPECT_NE(retention.find("\"pruned_count\":\"18\""), std::string::npos);
    EXPECT_NE(retention.find("\"duration_guaranteed\":false"), std::string::npos);
    HistorianQuery query;
    query.severities = {"ERROR"};
    query.search = "gripper fault";
    ASSERT_EQ(store.Query(query).events.size(), 1U);
  }
  SqliteEventHistoryStore restarted(directory.path(), kStart, 2U, TriggerOnlyCaptureLimits());
  EXPECT_TRUE(restarted.Read(1U));
  EXPECT_NE(content_identity::canonical_json(restarted.Query(HistorianQuery{}).retention)
      .find("\"pruned_count\":\"18\""), std::string::npos);
}

TEST(EventHistorian, PayloadByteRetentionIsBoundedIndependentlyOfRowCapacity) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart, 100000U);
  for (std::uint64_t index = 1; index <= 68; ++index) {
    ASSERT_EQ(store.Append(MakeHistorianEvent("manyforge.runtime", "operation_progressed", "INFO",
        static_cast<std::int64_t>(index), "2026-09-05T00:00:00Z",
        CanonicalValue(CanonicalValue::Object{{"data", CanonicalValue(std::string(250000, 'x'))}}))), IEventHistoryStore::AppendStatus::kStored);
  }
  // 68 * 250011 exceeds the 16 MiB routine budget: exactly one old row is pruned.
  EXPECT_FALSE(store.Read(1U));
  EXPECT_TRUE(store.Read(2U));
  HistorianQuery empty;
  empty.components = {"absent"};
  const auto retention = content_identity::canonical_json(store.Query(empty).retention);
  EXPECT_NE(retention.find("\"pruned_count\":\"1\""), std::string::npos);
  EXPECT_NE(retention.find("\"retained_count\":\"67\""), std::string::npos);
}

TEST(EventHistorian, IndexedSearchReflectsPruningAfterRestart) {
  TemporaryDirectory directory;
  const auto event = [](std::int64_t index, const std::string &tag) {
    return MakeHistorianEvent(
        "manyforge.runtime", "operation_progressed", "INFO", index,
        "2026-09-05T00:00:00Z",
        CanonicalValue(CanonicalValue::Object{{"message", CanonicalValue(tag)}}));
  };
  const auto check_search = [](SqliteEventHistoryStore &store) {
    HistorianQuery query;
    query.search = "retiredneedle";
    EXPECT_TRUE(store.Query(query).events.empty());
    query.search = "retainedneedle";
    const auto page = store.Query(query);
    ASSERT_EQ(page.events.size(), 1U);
    EXPECT_EQ(page.events.front().sequence, 3U);
  };
  {
    SqliteEventHistoryStore store(directory.path(), kStart, 2U);
    ASSERT_EQ(store.Append(event(1, "retiredneedle")), IEventHistoryStore::AppendStatus::kStored);
    ASSERT_EQ(store.Append(event(2, "other")), IEventHistoryStore::AppendStatus::kStored);
    ASSERT_EQ(store.Append(event(3, "retainedneedle")), IEventHistoryStore::AppendStatus::kStored);
    EXPECT_FALSE(store.Read(1U));
    check_search(store);
  }
  SqliteEventHistoryStore reopened(directory.path(), kStart, 2U);
  check_search(reopened);
}

TEST(EventHistorian, DerivedIndexTamperIsNotPresentedAsAnIntactRecord) {
  TemporaryDirectory directory;
  const auto path = SqliteEventHistoryStore::DatabasePath(directory.path());
  { SqliteEventHistoryStore store(directory.path(), kStart, 2U); ASSERT_EQ(store.Append(Event(1)), IEventHistoryStore::AppendStatus::kStored); }
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, "UPDATE historian_events SET condition_severity='ERROR'", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  ExpectHistorianError([&] { SqliteEventHistoryStore store(directory.path(), kStart, 2U); }, "historian_storage_corrupt");
}

TEST(EventHistorian, SecondaryIndexTamperIsRejectedBeforeQueries) {
  TemporaryDirectory directory;
  for (const bool search : {false, true}) {
    const auto history = HistoryDirectory(directory, search ? "search" : "correlation");
    const auto path = SqliteEventHistoryStore::DatabasePath(history);
    { SqliteEventHistoryStore store(history, kStart, 8U); ASSERT_EQ(store.Append(Event(1)), IEventHistoryStore::AppendStatus::kStored); }
    sqlite3 *database = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    const std::string sql = search
        ? std::string("INSERT INTO historian_search(rowid,component,event_type,details) VALUES(99,'fake','fake','gripper phantom')")
        : "INSERT INTO historian_event_keys(sequence,kind,value,application_start_id) VALUES(1,'operation_id','operation.fake','" +
              kStart + "')";
    ASSERT_EQ(sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    ExpectHistorianError([&] { SqliteEventHistoryStore store(history, kStart, 8U); }, "historian_storage_corrupt");
  }
}

TEST(EventHistorian, DiagnosticLossCountersCannotAuthorizeDeletingRetainedEvidence) {
  TemporaryDirectory directory;
  const auto path = SqliteEventHistoryStore::DatabasePath(directory.path());
  { SqliteEventHistoryStore store(directory.path(), kStart, 8U);
    ASSERT_EQ(store.Append(Event(1)), IEventHistoryStore::AppendStatus::kStored);
    ASSERT_EQ(store.Append(Event(2)), IEventHistoryStore::AppendStatus::kStored); }
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  // This is only diagnostic provenance. There is no persistent usage counter
  // whose corruption can tell the writer to delete an intact record.
  ASSERT_EQ(sqlite3_exec(database, "UPDATE historian_retention SET pruned_count=10", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  SqliteEventHistoryStore reopened(directory.path(), kStart, 8U);
  ASSERT_EQ(reopened.Append(Event(3)), IEventHistoryStore::AppendStatus::kStored);
  EXPECT_EQ(reopened.Query(HistorianQuery{}).events.size(), 3U);
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, "UPDATE historian_retention SET pruned_count=11", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  ExpectHistorianError([&] { static_cast<void>(reopened.Append(Event(4))); }, "historian_storage_changed");
}

TEST(EventHistorian, SearchCoversHumanTextAndIdentitiesButNeverPayloadJson) {
  TemporaryDirectory directory;
  const auto event = MakeHistorianEvent(
      "manyforge.runtime", "stop_event_completed", "WARN", 1, "2026-10-28T00:00:00Z",
      CanonicalValue(CanonicalValue::Object{
          {"message", CanonicalValue("Gripper closed on \"part\"")},
          {"reason_code", CanonicalValue("grip_lost")},
          {"details", CanonicalValue(CanonicalValue::Object{
                          {"reason", CanonicalValue("finger_blocked")},
                          {"reasons", CanonicalValue(CanonicalValue::Array{CanonicalValue("sensor_stale"),
                                                                           CanonicalValue("quoted \"jaw\" reason")})},
                          {"note", CanonicalValue("hiddenneedle")}})},
          {"subject", CanonicalValue(CanonicalValue::Object{
                          {"subject_id", CanonicalValue("cell.alpha")},
                          {"resource_ids", CanonicalValue(CanonicalValue::Array{CanonicalValue("arm.res1"),
                                                                                CanonicalValue("arm\\joint2")})}})},
          {"journal_payload", CanonicalValue(CanonicalValue::Object{
                                  {"stop_event_id", CanonicalValue(42042.0)},
                                  {"terminal_error", CanonicalValue(CanonicalValue::Object{
                                                         {"code", CanonicalValue("ik_failed")},
                                                         {"message", CanonicalValue("nestedneedle")}})}})},
          {"payload_digest", CanonicalValue("sha256:abcdef0123")}}));
  const auto found = [](SqliteEventHistoryStore &store, const std::string &text) {
    HistorianQuery query;
    query.search = text;
    return store.Query(query).events.size();
  };
  for (const bool reopened : {false, true}) {
    SqliteEventHistoryStore store(directory.path(), kStart, 8U);
    if (!reopened) {
      ASSERT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
    }
    for (const auto *text : {"manyforge.runt", "event_compl", "closed on \"part\"", "grip_lost", "finger_blocked",
                             "sensor_stale", "cell.alpha", "arm.res1", "42042", "ik_failed",
                             // Array elements: literal quote and backslash.
                             "quoted \"jaw\" reason", "arm\\joint2"})
      EXPECT_EQ(found(store, text), 1U) << text;
    for (const auto *text : {"hiddenneedle", "nestedneedle", "sha256:abc", "payload_digest", "journal_payload"})
      EXPECT_EQ(found(store, text), 0U) << text;
  }
}

TEST(EventHistorian, DiscoveryAndExportUseBoundedExactContinuations) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart);
  for (int i = 0; i < 5; ++i) {
    (void)store.Append(MakeHistorianEvent("manyforge.runtime", "operation_progressed", "INFO", i,
        "2026-09-06T12:00:00Z", CanonicalValue(CanonicalValue::Object{
            {"blob", CanonicalValue(std::string(250000, 'x'))},
            {"program_run_id", CanonicalValue(static_cast<double>(i + 1))}})));
  }
  const auto discovery = store.Discover("program_run_id", "", 2);
  EXPECT_EQ(discovery.find_object_member("values")->as_array().size(), 2U);
  EXPECT_EQ(discovery.find_object_member("next_after")->as_string(), "2");
  EXPECT_EQ(store.Discover("program_run_id", "4", 2).find_object_member("values")->as_array().size(), 1U);
  ExpectHistorianError([&] { static_cast<void>(store.Discover("payload_json", "", 32)); }, "historian_query_invalid");
  HistorianQuery query;
  query.limit = 32;
  const auto exported = HistorianExportPageValue(store.QueryExport(query));
  const auto &rows = exported.find_object_member("events")->as_array();
  ASSERT_EQ(rows.size(), 4U); // the byte bound, not only the row count, owns this page
  EXPECT_TRUE(std::get<bool>(exported.find_object_member("truncated")->storage()));
  EXPECT_EQ(std::get<double>(exported.find_object_member("next_before_sequence")->storage()), 2.0);
  EXPECT_LE(content_identity::canonical_json(exported).size(), 1024U * 1024U);
  const auto &event = rows.front();
  EXPECT_EQ(event.find_object_member("persistence_digest")->as_string(),
            content_identity::sha256_digest(content_identity::canonical_json(HistorianPersistenceValue(store.Read(5)->event))));
}

TEST(EventHistorian, UnselectableHistoricalReferenceValuesCannotStrandDiscovery) {
  TemporaryDirectory directory;
  for (const bool reopened : {false, true}) {
    SqliteEventHistoryStore store(directory.path(), kStart);
    if (!reopened) {
      for (const auto &value : {std::string(""), std::string(2048, 'a'), std::string("run.valid")}) {
        (void)store.Append(MakeHistorianEvent("manyforge.runtime", "diagnostic", "INFO", 1,
            "2026-09-06T12:00:00Z", CanonicalValue(CanonicalValue::Object{{"program_run_id", CanonicalValue(value)}})));
      }
    }
    const auto discovered = store.Discover("program_run_id", "", 1);
    ASSERT_EQ(discovered.find_object_member("values")->as_array().size(), 1U);
    EXPECT_EQ(discovered.find_object_member("values")->as_array().front().as_string(), "run.valid");
    EXPECT_FALSE(std::get<bool>(discovered.find_object_member("truncated")->storage()));
    EXPECT_EQ(store.Query(HistorianQuery{}).events.size(), 3U); // original evidence remains readable
  }
}

TEST(EventHistorian, ChannelIsTheExactDisplayedComponentNotNestedPayloadText) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart);
  for (const auto &component : {"manyforge.runtime", "manyforge.runtime.child"}) {
    (void)store.Append(MakeHistorianEvent(component, "progress", "INFO", 1,
        "2026-09-06T12:00:00Z", CanonicalValue(CanonicalValue::Object{
          {"channel", CanonicalValue("unrelated.payload.channel")}})));
  }
  const auto channels = store.Discover("channel", "", 32);
  ASSERT_EQ(channels.find_object_member("values")->as_array().size(), 2U);
  EXPECT_EQ(channels.find_object_member("values")->as_array().front().as_string(), "manyforge.runtime");
  HistorianQuery query;
  query.correlations.emplace("channel", "manyforge.runtime");
  const auto page = store.Query(query);
  ASSERT_EQ(page.events.size(), 1U);
  EXPECT_EQ(page.events.front().event.component, "manyforge.runtime");
  query.correlations["channel"] = "unrelated.payload.channel";
  EXPECT_TRUE(store.Query(query).events.empty());
}

TEST(EventHistorian, BusyAndReadonlyStorageNeverClaimsSuccessfulPersistence) {
  TemporaryDirectory directory;
  const auto path = SqliteEventHistoryStore::DatabasePath(directory.path());
  {
    SqliteEventHistoryStore store(directory.path(), kStart, 2U);
    sqlite3 *blocker = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &blocker), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
    ExpectHistorianError([&] { static_cast<void>(store.Append(Event(1))); }, "historian_storage_busy");
    ASSERT_EQ(sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(blocker), SQLITE_OK);
    EXPECT_EQ(store.Append(Event(1)), IEventHistoryStore::AppendStatus::kStored);
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_read);
  ExpectHistorianError([&] { SqliteEventHistoryStore readonly(directory.path(), kStart, 2U); }, "historian_storage_failure");
  std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  SqliteEventHistoryStore recovered(directory.path(), kStart, 2U);
  EXPECT_TRUE(recovered.Read(1U));
}

TEST(EventHistorian, SqlitePageLimitReportsStorageFullAndKeepsCommittedEvents) {
  TemporaryDirectory directory;
  ScopedTestDatabaseCapture capture;
  ASSERT_EQ(capture.result(), SQLITE_OK);
  SqliteEventHistoryStore store(directory.path(), kStart, 100U);
  ASSERT_NE(captured_test_database, nullptr);
  ASSERT_EQ(store.Append(Event(1U)), IEventHistoryStore::AppendStatus::kStored);

  // SQLite clamps the requested limit to the current page count. The large
  // second event must allocate more pages and returns SQLite's real FULL.
  ASSERT_EQ(sqlite3_exec(captured_test_database, "PRAGMA max_page_count=1", nullptr,
                         nullptr, nullptr), SQLITE_OK);
  const auto second = MakeHistorianEvent(
      "manyforge.runtime", "observation", "INFO", 2, "2026-09-06T12:00:00Z",
      CanonicalValue(CanonicalValue::Object{{"blob", CanonicalValue(std::string(250000U, 'x'))}}));
  ExpectHistorianError([&] { static_cast<void>(store.Append(second)); },
                       "historian_storage_full");
  EXPECT_TRUE(store.Read(1U));
  EXPECT_FALSE(store.Read(2U));

  ASSERT_EQ(sqlite3_exec(captured_test_database, "PRAGMA max_page_count=131072", nullptr,
                         nullptr, nullptr), SQLITE_OK);
  EXPECT_EQ(store.Append(second), IEventHistoryStore::AppendStatus::kStored);
  EXPECT_TRUE(store.Read(2U));
}

TEST(EventHistorian, BatchCommitsTheEventsBeforeAFailureAndRollsBackOnlyTheFailingEvent) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart, 100U);
  ASSERT_EQ(store.Append(Event(1U)), IEventHistoryStore::AppendStatus::kStored);
  auto conflicting = Event(1U);
  conflicting.payload = CanonicalValue(CanonicalValue::Object{{"index", CanonicalValue("other")}});
  const std::vector<HistorianEvent> batch{Event(2U), Event(1U), conflicting, Event(3U)};
  const auto result = store.AppendBatch(batch);
  ASSERT_EQ(result.statuses, (std::vector<IEventHistoryStore::AppendStatus>{
                                 IEventHistoryStore::AppendStatus::kStored,
                                 IEventHistoryStore::AppendStatus::kDuplicate}));
  ASSERT_TRUE(result.failure);
  ExpectHistorianError([&] { std::rethrow_exception(result.failure); },
                       "historian_event_identity_conflict");
  EXPECT_EQ(store.Read(2U)->event.event_id, Event(2U).event_id);
  EXPECT_FALSE(store.Read(3U));
  EXPECT_EQ(store.Append(Event(3U)), IEventHistoryStore::AppendStatus::kStored);
  EXPECT_EQ(store.Read(3U)->event.event_id, Event(3U).event_id);
}

TEST(EventHistorian, OneTransactionTakesABoundedAmountOfRecordBytes) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart, 100000U);
  std::vector<HistorianEvent> batch;
  for (std::int64_t index = 1; index <= 40; ++index) {
    batch.push_back(MakeHistorianEvent("manyforge.runtime", "observation", "INFO", index,
                                       "2026-09-24T12:00:00Z",
                                       CanonicalValue(CanonicalValue::Object{
                                           {"blob", CanonicalValue(std::string(200000U, 'x'))}})));
  }
  const auto first = store.AppendBatch(batch);
  EXPECT_FALSE(first.failure);
  // 4 MiB of records: the event that reaches the bound is the last one taken.
  EXPECT_EQ(first.statuses.size(), 21U);
  const auto rest = store.AppendBatch(std::span<const HistorianEvent>(batch).subspan(first.statuses.size()));
  EXPECT_FALSE(rest.failure);
  EXPECT_EQ(rest.statuses.size(), 19U);
  EXPECT_TRUE(store.Read(40U));
}

TEST(EventHistorian, PinnedWriteAheadLogStopsWritesAtItsBoundInsteadOfGrowing) {
  TemporaryDirectory directory;
  const auto path = SqliteEventHistoryStore::DatabasePath(directory.path());
  const auto wal = std::filesystem::path(path.string() + "-wal");
  const auto large = [](const std::int64_t index) {
    return MakeHistorianEvent("manyforge.runtime", "observation", "INFO", index,
                              "2026-09-06T12:00:00Z",
                              CanonicalValue(CanonicalValue::Object{
                                  {"blob", CanonicalValue(std::string(200000U, 'x'))}}));
  };
  SqliteEventHistoryStore store(directory.path(), kStart, 100000U);
  ASSERT_EQ(store.Append(large(1)), IEventHistoryStore::AppendStatus::kStored);
  // A foreign reader holding a snapshot prevents reclaiming the log.
  sqlite3 *reader = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &reader), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(reader, "BEGIN; SELECT count(*) FROM historian_events;", nullptr, nullptr, nullptr),
            SQLITE_OK);
  bool refused = false;
  for (std::int64_t index = 2; index <= 2000 && !refused; ++index) {
    try {
      static_cast<void>(store.Append(large(index)));
    } catch (const EventHistorianError &error) {
      EXPECT_EQ(error.code(), "historian_storage_busy");
      refused = true;
    }
  }
  EXPECT_TRUE(refused);
  // Bounded: 64 MiB of unreclaimed log plus at most one batch.
  EXPECT_LE(std::filesystem::file_size(wal), 72U * 1024U * 1024U);
  ASSERT_EQ(sqlite3_exec(reader, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(reader), SQLITE_OK);
  EXPECT_EQ(store.Append(large(5000)), IEventHistoryStore::AppendStatus::kStored);
  EXPECT_LE(std::filesystem::file_size(wal), 8U * 1024U * 1024U);
}

TEST(EventHistorian, SummaryPageSelectsListFieldsWithoutPublishingPayload) {
  const auto event = MakeHistorianEvent(
      "manyforge.composer", "client_incident_activated", "ERROR",
      9007199254740992LL, "2026-09-04T10:00:00Z",
      CanonicalValue(CanonicalValue::Object{
          {"incident_kind", CanonicalValue("control_delayed")},
          {"nested",
           CanonicalValue(CanonicalValue::Object{
               {"diagnostic_blob", CanonicalValue(std::string(32000U, 'x'))},
               {"operation_id", CanonicalValue(9.0)},
           })},
          {"phase", CanonicalValue("resolved")},
          {"program_run_id", CanonicalValue(7.0)},
      }));
  const auto encoded =
      content_identity::canonical_json(HistorianSummaryPageValue(
          HistorianPage{{HistorianStoredEvent{7U, event}}, true, 6U}));

  EXPECT_NE(encoded.find("manyforge.historian_summary_page.v3"),
            std::string::npos);
  EXPECT_NE(encoded.find("manyforge.historian_event_summary.v3"),
            std::string::npos);
  EXPECT_NE(encoded.find("Control delayed resolved"), std::string::npos);
  EXPECT_NE(encoded.find("\"program_run_id\":\"7\""), std::string::npos);
  EXPECT_NE(encoded.find("\"operation_id\":\"9\""), std::string::npos);
  EXPECT_NE(encoded.find("\"occurred_at_ns\":\"9007199254740992\""),
            std::string::npos);
  EXPECT_EQ(encoded.find("diagnostic_blob"), std::string::npos);
  EXPECT_EQ(encoded.find(std::string(1024U, 'x')), std::string::npos);
}

TEST(EventHistorian,
     OperationFailureSummaryUsesOriginalErrorAndAttemptWithoutExtraRows) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart);
  const auto event = OperationFailureEvent("No path from authored target");
  EXPECT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
  const auto summary =
      HistorianSummaryEventValue(event, std::nullopt, std::nullopt, false);
  EXPECT_EQ(
      summary.find_object_member("message")->as_string(),
      "[move / 1] planning_failed: No path from authored target");
  EXPECT_EQ(summary.find_object_member("severity")->as_string(), "ERROR");
  EXPECT_EQ(summary.find_object_member("operation_id")->as_string(), "1");
  EXPECT_EQ(summary.find_object_member("program_run_id")->as_string(), "1");
  const auto page = store.Query({});
  ASSERT_EQ(page.events.size(), 1U);
  EXPECT_EQ(page.events.front().event.payload, event.payload);
  const auto detail = content_identity::canonical_json(
      HistorianDetailValue(page.events.front()));
  EXPECT_NE(detail.find("planning_failed"), std::string::npos);
  EXPECT_NE(detail.find("\"node_attempt_id\":1"), std::string::npos);
}

TEST(EventHistorian,
     OperationFailureSummaryIsBoundedAndPreservesLegacyProjection) {
  const auto long_event = OperationFailureEvent(std::string(9000U, 'x'));
  const auto summary =
      HistorianSummaryEventValue(long_event, std::nullopt, std::nullopt, false);
  const auto &message = summary.find_object_member("message")->as_string();
  EXPECT_EQ(message.size(), 1024U);
  EXPECT_EQ(message.find("[move / 1] planning_failed: "), 0U);
  // The bounded list projection never clips the source evidence in
  // detail/export.
  EXPECT_EQ(long_event.payload.find_object_member("journal_payload")
                ->find_object_member("operation_state")
                ->find_object_member("participants")
                ->as_array()
                .front()
                .find_object_member("terminal_error")
                ->find_object_member("message")
                ->as_string()
                .size(),
            9000U);
  auto legacy_payload = long_event.payload.as_object();
  std::erase_if(legacy_payload,
                [](const auto &field) { return field.first == "message"; });
  const auto legacy = MakeHistorianEvent(
      long_event.component, long_event.event_type, long_event.severity,
      long_event.occurred_at_ns, long_event.occurred_at_wall,
      CanonicalValue(std::move(legacy_payload)));
  const auto legacy_summary =
      HistorianSummaryEventValue(legacy, std::nullopt, std::nullopt, false);
  EXPECT_EQ(legacy_summary.find_object_member("message")->as_string(),
            "motion retry selected");
  TemporaryDirectory directory;
  {
    SqliteEventHistoryStore store(directory.path(), kStart);
    EXPECT_EQ(store.Append(legacy), IEventHistoryStore::AppendStatus::kStored);
  }
  SqliteEventHistoryStore reopened(directory.path(), kStart);
  EXPECT_EQ(reopened.Query({}).events.size(), 1U);
  EXPECT_EQ(reopened.Read(1)->event.payload, legacy.payload);
}

TEST(EventHistorian,
     OperationFailureMessageKeepsCoordinatedErrorsWithTheirOwnScopes) {
  const auto participant = [](const char *node, const char *code,
                              const char *message) {
    return CanonicalValue(CanonicalValue::Object{
        {"execution_scope",
         CanonicalValue(CanonicalValue::Object{
             {"node_instance_id", CanonicalValue(node)},
             {"node_attempt_id", CanonicalValue(1.0)}})},
        {"terminal_error", CanonicalValue(CanonicalValue::Object{
                               {"code", CanonicalValue(code)},
                               {"message", CanonicalValue(message)}})}});
  };
  const CanonicalValue state(CanonicalValue::Object{
      {"participants", CanonicalValue(CanonicalValue::Array{
                           participant("peer", "cancelled", "Peer stopped"),
                           participant("origin", "runtime_collision_fault",
                                       "Collision detected")})}});
  EXPECT_EQ(OperationFailureHistoryMessage(state),
            "[peer / 1] cancelled: Peer stopped; "
            "[origin / 1] runtime_collision_fault: Collision "
            "detected");
  EXPECT_TRUE(OperationFailureHistoryMessage(CanonicalValue(nullptr)).empty());
}

TEST(EventHistorian, StoredSummariesMatchFullProjectionAcrossFiltersAndPages) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart, 2U);
  for (int i = 1; i <= 6; ++i) {
    (void)store.Append(MakeHistorianEvent(
        "manyforge.test", "connection_changed", i % 2 ? "ERROR" : "INFO", i,
        i == 6 ? "2026-09-08T00:00:00Z" : "2026-09-08T00:00:01Z",
        CanonicalValue(CanonicalValue::Object{{"clock_owner", CanonicalValue("owner.1")},
                                              {"program_run_id", CanonicalValue(3.0)},
                                              {"message", CanonicalValue("searchable message")}})));
  }
  HistorianQuery query;
  query.limit = 1;
  for (int filter = 0; filter < 7; ++filter) {
    if (filter == 1) query.components = {"manyforge."};
    if (filter == 2) query.event_types = {"connection_changed"};
    if (filter == 3) query.correlations = {{"program_run_id", "3"}};
    if (filter == 4) query.search = "searchable";
    if (filter == 5) query.severities = {"ERROR"};
    if (filter == 6) query.clock_owner = "owner.1";
    query.before_sequence.reset();
    do {
      const auto full = store.Query(query);
      EXPECT_EQ(content_identity::canonical_json(store.QuerySummaries(query)),
                content_identity::canonical_json(HistorianSummaryPageValue(full)));
      query.before_sequence = full.next_before_sequence;
    } while (query.before_sequence);
  }
}

TEST(EventHistorian, TamperedSummaryCannotReplaceAnOriginalEvenWithARecomputedDigest) {
  TemporaryDirectory directory;
  for (const bool rehashed : {false, true}) {
    const auto history = HistoryDirectory(directory, rehashed ? "rehashed" : "tampered");
    const auto path = SqliteEventHistoryStore::DatabasePath(history);
    const auto event = Event(1);
    {
      SqliteEventHistoryStore store(history, kStart);
      (void)store.Append(event);
    }
    const std::string summary = "{}";
    const auto event_digest = content_identity::sha256_digest(
        content_identity::canonical_json(HistorianPersistenceValue(event)));
    const auto digest = content_identity::sha256_digest("manyforge.historian-summary.v1\n1\n" +
                                                        event_digest + "\n" + summary);
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    const std::string sql = "UPDATE historian_events SET summary_json='{}'" +
                            (rehashed ? ",summary_digest='" + digest + "'" : "");
    ASSERT_EQ(sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    ExpectHistorianError([&] { SqliteEventHistoryStore rejected(history, kStart); },
                         "historian_storage_corrupt");
  }
}

TEST(EventHistorian, LargeStructuredPayloadsDoNotMakeListOrExportDecodeDiscardedRows) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart);
  const CanonicalValue payload(CanonicalValue::Object{
      {"data", CanonicalValue(CanonicalValue::Array(120000, CanonicalValue(0.0)))}});
  for (int i = 1; i <= 32; ++i) {
    (void)store.Append(MakeHistorianEvent("manyforge.test", "diagnostic", "INFO", i,
                                          "2026-09-08T00:00:00Z", payload));
  }
  HistorianQuery query;
  query.limit = 32;
  const auto summaries = store.QuerySummaries(query);
  EXPECT_EQ(summaries.find_object_member("events")->as_array().size(), 32U);
  EXPECT_LT(content_identity::canonical_json(summaries).size(), 32768U);
  std::uint64_t expected = 32;
  do {
    const auto page = store.QueryExport(query);
    ASSERT_FALSE(page.events.empty());
    EXPECT_LE(page.events.size(), 4U);
    for (const auto& row : page.events) EXPECT_EQ(row.sequence, expected--);
    const auto encoded = HistorianExportPageValue(page);
    EXPECT_EQ(encoded.find_object_member("events")->as_array().size(), page.events.size());
    EXPECT_LE(content_identity::canonical_json(encoded).size(), 1024U * 1024U);
    query.before_sequence = page.next_before_sequence;
  } while (query.before_sequence);
  EXPECT_EQ(expected, 0U);
  EXPECT_EQ(content_identity::canonical_json(store.Read(32)->event.payload),
            content_identity::canonical_json(payload));
}

TEST(EventHistorian, DetailPublishesOnlyTypedSourceBackedSections) {
  const auto event = MakeHistorianEvent(
      "manyforge.execution", "operation_updated", "ERROR", 101,
      "2026-08-05T00:00:00Z",
      CanonicalValue(CanonicalValue::Object{
          {"blackboard", CanonicalValue(CanonicalValue::Object{
                             {"part_ready", CanonicalValue(true)},
                         })},
          {"application_start_id", CanonicalValue(kStart)},
          {"duration_ns", CanonicalValue(1250.0)},
          {"operation_id", CanonicalValue(1.0)},
          {"record_kind", CanonicalValue("observation")},
          {"schema_version",
           CanonicalValue("manyforge.operation_history_event.v2")},
      }));
  const auto encoded = content_identity::canonical_json(
      HistorianDetailValue(HistorianStoredEvent{7U, event}));
  EXPECT_NE(encoded.find("manyforge.historian_detail.v3"), std::string::npos);
  EXPECT_NE(encoded.find("\"operation_id\":1"), std::string::npos);
  EXPECT_NE(encoded.find("\"application_start_id\":\"" + kStart + "\""), std::string::npos);
  EXPECT_NE(encoded.find("part_ready"), std::string::npos);
  EXPECT_NE(encoded.find("duration_ns"), std::string::npos);
  EXPECT_NE(encoded.find("incident"), std::string::npos);
}

TEST(EventHistorian, SqliteStoreRejectsAliasForeignIdentityAndTamper) {
  TemporaryDirectory directory;
  const auto target_directory = HistoryDirectory(directory, "target");
  const auto target = SqliteEventHistoryStore::DatabasePath(target_directory);
  {
    SqliteEventHistoryStore store(target_directory, kStart, 8U);
    EXPECT_EQ(store.Append(Event(1U)),
              IEventHistoryStore::AppendStatus::kStored);
  }
  const auto alias_directory = HistoryDirectory(directory, "alias");
  std::filesystem::create_symlink(target, SqliteEventHistoryStore::DatabasePath(alias_directory));
  ExpectHistorianError([&]() { SqliteEventHistoryStore store(alias_directory, kStart, 8U); },
                       "historian_path_invalid");

  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open_v2(target.string().c_str(), &database,
                            SQLITE_OPEN_READWRITE, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database,
                         "UPDATE historian_events SET payload_json='{}'",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  ExpectHistorianError(
      [&]() { SqliteEventHistoryStore tampered(target_directory, kStart, 8U); },
      "historian_storage_corrupt");

  const auto foreign_directory = HistoryDirectory(directory, "foreign");
  const auto foreign = SqliteEventHistoryStore::DatabasePath(foreign_directory);
  ASSERT_EQ(sqlite3_open_v2(foreign.string().c_str(), &database,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, "CREATE TABLE foreign_table(value TEXT)",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  ExpectHistorianError([&]() { SqliteEventHistoryStore store(foreign_directory, kStart, 8U); },
                       "historian_storage_foreign");
}

TEST(EventHistorian,
     AsyncQueueNeverWaitsForStorageAndOverflowDropsCountsAndEndsWhenWrittenEmpty) {
  auto store = std::make_shared<ControllableStore>();
  store->SetBlocked(true);
  testing::internal::CaptureStderr();
  AsyncEventHistorian historian(store, 2U);
  EXPECT_TRUE(historian.Health().available);
  const auto healthy_generation = historian.Health().health_generation;
  EXPECT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(store->WaitForAppendCalls(1U, 2s));

  EXPECT_TRUE(historian.TryEnqueue(Event(2U)));
  // The full queue drops and counts; neither call waits for storage.
  EXPECT_FALSE(historian.TryEnqueue(Event(3U)));
  EXPECT_FALSE(historian.TryEnqueue(Event(4U)));
  auto health = historian.Health();
  EXPECT_FALSE(health.available);
  EXPECT_EQ(health.error_code, "historian_queue_overflow");
  EXPECT_EQ(health.queue_depth, 2U);
  EXPECT_EQ(health.overflow_count, 2U);
  EXPECT_GT(health.health_generation, healthy_generation);
  // OD-215: the disk check's level.
  EXPECT_TRUE(historian.DiskSignals().queue_full);
  EXPECT_FALSE(historian.DiskSignals().storage_full);

  // Written empty again: the loss episode ends by itself.
  store->SetBlocked(false);
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  health = historian.Health();
  EXPECT_TRUE(health.available);
  EXPECT_FALSE(health.error_code.has_value());
  EXPECT_FALSE(health.error_message.has_value());
  EXPECT_EQ(health.overflow_count, 2U);
  EXPECT_EQ(store->StoredCount(), 2U);
  EXPECT_FALSE(store->Stored(Event(3U).event_id));
  EXPECT_FALSE(historian.DiskSignals().queue_full);
  historian.Shutdown();
  // One log line for the whole episode, however many records it dropped.
  const auto log = testing::internal::GetCapturedStderr();
  std::size_t lines = 0U;
  for (auto at = log.find("historian records dropped"); at != std::string::npos;
       at = log.find("historian records dropped", at + 1U)) {
    ++lines;
  }
  EXPECT_EQ(lines, 1U);
}

TEST(EventHistorian, FailedWriteDropsAndCountsItsRecordAndTheWriterContinues) {
  auto store = std::make_shared<ControllableStore>();
  store->SetFailing(true, "historian_storage_full");
  AsyncEventHistorian historian(store, 4U);
  EXPECT_TRUE(historian.TryEnqueue(Event(1U)));
  // The failed record is dropped, not retried: the queue is empty again.
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  const auto failed = historian.Health();
  EXPECT_FALSE(failed.available);
  EXPECT_EQ(failed.error_code, "historian_storage_full");
  EXPECT_EQ(failed.delivery_failure_count, 1U);
  EXPECT_EQ(failed.queue_depth, 0U);
  EXPECT_EQ(store->StoredCount(), 0U);
  // OD-215: a write that found the disk full; one record is not a full queue.
  EXPECT_TRUE(historian.DiskSignals().storage_full);
  EXPECT_FALSE(historian.DiskSignals().queue_full);

  // A loss stays reported until History writes again with an empty queue.
  store->SetFailing(false);
  EXPECT_FALSE(historian.Health().available);
  EXPECT_TRUE(historian.TryEnqueue(Event(2U)));
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  const auto recovered = historian.Health();
  EXPECT_TRUE(recovered.available);
  EXPECT_FALSE(recovered.error_code.has_value());
  EXPECT_EQ(recovered.delivery_failure_count, 1U);
  EXPECT_EQ(store->StoredCount(), 1U);
  EXPECT_FALSE(store->Stored(Event(1U).event_id));
  EXPECT_FALSE(historian.DiskSignals().storage_full);
}

// OD-215: "queue full" ends when the queue is empty again, even when its
// writes failed (the loss episode goes on): a store failing for another
// reason cannot hold the disk check's stop.
TEST(EventHistorian, QueueFullLevelEndsWhenTheQueueIsEmptyEvenIfWritesFail) {
  auto store = std::make_shared<ControllableStore>();
  store->SetBlocked(true);
  store->SetFailing(true);
  testing::internal::CaptureStderr();
  AsyncEventHistorian historian(store, 2U);
  EXPECT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(store->WaitForAppendCalls(1U, 2s));
  EXPECT_TRUE(historian.TryEnqueue(Event(2U)));
  EXPECT_FALSE(historian.TryEnqueue(Event(3U)));
  EXPECT_TRUE(historian.DiskSignals().queue_full);
  store->SetBlocked(false);
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  EXPECT_FALSE(historian.DiskSignals().queue_full);
  EXPECT_FALSE(historian.Health().available);
  historian.Shutdown();
  static_cast<void>(testing::internal::GetCapturedStderr());
}

TEST(EventHistorian, ARecordThatAlwaysFailsIsDroppedAloneAndLaterRecordsAreWritten) {
  auto store = std::make_shared<ControllableStore>();
  store->FailEvent(Event(2U).event_id);
  store->SetBlocked(true);
  AsyncEventHistorian historian(store, 8U);
  ASSERT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(store->WaitForAppendCalls(1U, 2s));
  // Queued behind the blocked write: they share the next batch, where the
  // record that fails rolls back alone and the one after it waits.
  ASSERT_TRUE(historian.TryEnqueue(Event(2U)));
  ASSERT_TRUE(historian.TryEnqueue(Event(3U)));
  store->SetBlocked(false);
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  const auto health = historian.Health();
  EXPECT_TRUE(store->Stored(Event(1U).event_id));
  EXPECT_FALSE(store->Stored(Event(2U).event_id));
  EXPECT_TRUE(store->Stored(Event(3U).event_id));
  EXPECT_EQ(health.delivery_failure_count, 1U);
  EXPECT_EQ(health.delivered_count, 2U);
  // Event 3 was written after the loss and left the queue empty.
  EXPECT_TRUE(health.available);
}

TEST(EventHistorian, AFailedWriteDropsTheWholeBatchItAttempted) {
  auto store = std::make_shared<ControllableStore>();
  store->SetBlocked(true);
  AsyncEventHistorian historian(store, 8U);
  ASSERT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(store->WaitForAppendCalls(1U, 2s));
  for (std::uint64_t index = 2U; index <= 4U; ++index) {
    ASSERT_TRUE(historian.TryEnqueue(Event(index)));
  }
  // The blocked write of event 1 completes; the next write fails as a whole.
  store->SetBatchThrows(true);
  store->SetBlocked(false);
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  auto health = historian.Health();
  EXPECT_FALSE(health.available);
  EXPECT_EQ(health.error_code, "historian_storage_busy");
  EXPECT_EQ(health.delivery_failure_count, 3U);
  EXPECT_EQ(health.queue_depth, 0U);
  EXPECT_EQ(store->StoredCount(), 1U);

  store->SetBatchThrows(false);
  ASSERT_TRUE(historian.TryEnqueue(Event(5U)));
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  health = historian.Health();
  EXPECT_TRUE(health.available);
  EXPECT_EQ(store->StoredCount(), 2U);
  EXPECT_TRUE(store->Stored(Event(5U).event_id));
}

TEST(EventHistorian, WriterCommitsWhatArrivesWithinTheGapTogether) {
  auto store = std::make_shared<ControllableStore>();
  AsyncEventHistorian historian(store, 64U, 200ms);
  ASSERT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  for (std::uint64_t index = 2U; index <= 11U; ++index) {
    ASSERT_TRUE(historian.TryEnqueue(Event(index)));
  }
  // A repeated identity still waiting in the queue is recognized by content.
  ASSERT_TRUE(historian.TryEnqueue(Event(11U)));
  auto conflicting = Event(11U);
  conflicting.payload = CanonicalValue(CanonicalValue::Object{{"index", CanonicalValue("other")}});
  EXPECT_THROW(static_cast<void>(historian.TryEnqueue(conflicting)), EventHistorianError);
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  EXPECT_EQ(store->BatchSizes(), (std::vector<std::size_t>{1U, 10U}));
  EXPECT_EQ(store->StoredCount(), 11U);
  const auto health = historian.Health();
  EXPECT_EQ(health.delivered_count, 11U);
  EXPECT_EQ(health.duplicate_count, 1U);
  EXPECT_TRUE(health.available);
}

TEST(EventHistorian, ProjectionFailureIsCountedAsALossThatTheNextWriteEnds) {
  auto store = std::make_shared<ControllableStore>();
  AsyncEventHistorian historian(store, 2U);
  historian.ReportDeliveryFailure("historian_projection_failure", "payload too large");
  const auto failure = historian.Health();
  EXPECT_FALSE(failure.available);
  EXPECT_EQ(failure.error_code, "historian_projection_failure");
  EXPECT_EQ(failure.delivery_failure_count, 1U);
  ASSERT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  const auto recovered = historian.Health();
  EXPECT_TRUE(recovered.available);
  EXPECT_FALSE(recovered.error_code.has_value());
  EXPECT_GT(recovered.health_generation, failure.health_generation);
  EXPECT_EQ(recovered.delivery_failure_count, 1U);
}

TEST(EventHistorian, AlarmLifecycleEnvelopeRetainsOwnerCallerAndSubject) {
  AlarmLifecycleFact fact{
      "operator_alarm_acknowledged",
      "manyforge.scene.pose_apply_fault",
      "condition-one",
      "activation-one",
      AlarmSubject{"object", "box", {"resource.arm.a"}},
      AlarmLifecycleState::kActiveUnacknowledged,
      AlarmLifecycleState::kActiveAcknowledged,
      2U,
      7U,
      "alarm.owner.epoch.1",
      100U,
      "2026-08-05T00:00:00Z",
      "scene.source",
      "scene.epoch.1",
      3U,
      AlarmCallerAttribution{"0123456789abcdef0123456789abcdef",
                             "operator_ui", "test", "manyforge.test.alarm",
                             "authenticated:test", "principal.operator"},
      "inspected",
      CanonicalValue(CanonicalValue::Object{}),
      CanonicalValue(CanonicalValue::Object{}),
  };
  const auto event = MakeAlarmLifecycleHistorianEvent(fact);
  const auto encoded =
      content_identity::canonical_json(HistorianEventValue(event));
  EXPECT_EQ(event.component, "manyforge.alarm");
  EXPECT_EQ(event.severity, "INFO");
  EXPECT_NE(encoded.find("resource.arm.a"), std::string::npos);
  EXPECT_NE(encoded.find("principal.operator"), std::string::npos);
  EXPECT_NE(encoded.find("alarm.owner.epoch.1"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Failure capture: bounded protection of already-durable rows around errors.

HistorianEvent RoutineEvent(const std::uint64_t index) {
  return Event(index, "manyforge.runtime", "operation_progressed");
}

HistorianEvent ErrorEvent(const std::uint64_t index) {
  return MakeHistorianEvent(
      "manyforge.runtime", "execute_failed", "ERROR",
      static_cast<std::int64_t>(100U + index), "2026-08-05T00:00:00Z",
      CanonicalValue(CanonicalValue::Object{
          {"index", CanonicalValue(static_cast<double>(index))},
          {"message", CanonicalValue("controller rejected trajectory")},
      }));
}

HistorianCaptureLimits SmallCaptureLimits() {
  HistorianCaptureLimits limits;
  limits.preceding_rows = 3U;
  limits.preceding_bytes = 1024LL * 1024LL;
  limits.tail_rows = 2U;
  limits.tail_duration = 1000ms;
  limits.capture_bytes = 2LL * 1024LL * 1024LL;
  limits.live_captures = 2U;
  limits.protected_bytes = 8LL * 1024LL * 1024LL;
  limits.recorded_triggers = 2U;
  return limits;
}

struct FakeWriterClock {
  std::int64_t now_ns{1'000'000'000};
  HistorianWriterClock Clock() {
    return [this] { return now_ns; };
  }
};

const CanonicalValue &Field(const CanonicalValue &value, std::string_view name) {
  const auto *field = value.find_object_member(name);
  if (field == nullptr) throw std::runtime_error("missing field " + std::string(name));
  return *field;
}

std::vector<CanonicalValue> CaptureList(SqliteEventHistoryStore &store) {
  const auto page = store.Captures(std::nullopt, 32U);
  return Field(page, "captures").as_array();
}

std::string CaptureState(const std::filesystem::path &path, std::int64_t capture_id) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    throw std::runtime_error("capture state read failed");
  }
  sqlite3_stmt *statement = nullptr;
  sqlite3_prepare_v2(database, "SELECT state FROM historian_captures WHERE capture_id=?", -1, &statement, nullptr);
  sqlite3_bind_int64(statement, 1, capture_id);
  std::string state = sqlite3_step(statement) == SQLITE_ROW
      ? reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)) : "absent";
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return state;
}

TEST(EventHistorian, FailureCaptureRetainsItsCauseAndBoundedContextAcrossPartitions) {
  TemporaryDirectory directory;
  FakeWriterClock clock;
  SqliteEventHistoryStore store(directory.path(), kStart, 4U, SmallCaptureLimits(),
                                clock.Clock());
  for (std::uint64_t index = 1; index <= 6; ++index) ASSERT_EQ(store.Append(RoutineEvent(index)), IEventHistoryStore::AppendStatus::kStored);
  ASSERT_EQ(store.Append(ErrorEvent(7)), IEventHistoryStore::AppendStatus::kStored);
  for (std::uint64_t index = 8; index <= 30; ++index) ASSERT_EQ(store.Append(RoutineEvent(index)), IEventHistoryStore::AppendStatus::kStored);

  const auto captures = CaptureList(store);
  ASSERT_EQ(captures.size(), 1U);
  const auto &capture = captures.front();
  EXPECT_EQ(Field(capture, "capture_id").as_string(), "1");
  EXPECT_EQ(Field(capture, "state").as_string(), "closed");
  EXPECT_EQ(Field(capture, "close_reason").as_string(), "tail_rows");
  EXPECT_EQ(Field(capture, "first_sequence").as_number(), 4.0);
  EXPECT_EQ(Field(capture, "trigger_sequence").as_number(), 7.0);
  EXPECT_EQ(Field(capture, "last_sequence").as_number(), 9.0);
  EXPECT_EQ(Field(capture, "preceding_limit").as_string(), "rows");
  EXPECT_EQ(Field(capture, "retained_rows").as_number(), 6.0);
  EXPECT_EQ(Field(capture, "coverage").as_string(), "complete");
  EXPECT_EQ(Field(Field(capture, "trigger"), "event_type").as_string(), "execute_failed");

  // Routine churn pruned everything else but never the protected window or
  // its (important-partition) cause.
  for (std::uint64_t sequence = 4; sequence <= 9; ++sequence) EXPECT_TRUE(store.Read(sequence)) << sequence;
  EXPECT_FALSE(store.Read(3U));
  EXPECT_FALSE(store.Read(10U));
  HistorianQuery query;
  query.capture_id = 1U;
  const auto page = store.Query(query);
  ASSERT_EQ(page.events.size(), 6U);
  EXPECT_EQ(page.events.front().sequence, 9U);
  EXPECT_EQ(page.events.back().sequence, 4U);
  const auto exported = content_identity::canonical_json(HistorianExportPageValue(store.QueryExport(query)));
  EXPECT_NE(exported.find("\"contents\":\"retained_event_context\""), std::string::npos);
  EXPECT_NE(exported.find("\"capture_id\":\"1\""), std::string::npos);
  const auto retention = content_identity::canonical_json(page.retention);
  EXPECT_NE(retention.find("\"schema_version\":\"manyforge.historian_retention.v3\""), std::string::npos);
  EXPECT_NE(retention.find("\"protected_count\":\"6\""), std::string::npos);
  query.capture_id = 99U;
  ExpectHistorianError([&] { static_cast<void>(store.Query(query)); }, "historian_capture_not_found");
}

TEST(EventHistorian, FailureBurstJoinsTheOpenCaptureWithoutExtendingItsLimits) {
  TemporaryDirectory directory;
  FakeWriterClock clock;
  SqliteEventHistoryStore store(directory.path(), kStart, 100U, SmallCaptureLimits(),
                                clock.Clock());
  ASSERT_EQ(store.Append(RoutineEvent(1)), IEventHistoryStore::AppendStatus::kStored);
  for (std::uint64_t index = 2; index <= 5; ++index) ASSERT_EQ(store.Append(ErrorEvent(index)), IEventHistoryStore::AppendStatus::kStored);
  const auto captures = CaptureList(store);
  ASSERT_EQ(captures.size(), 2U);  // Newest first.
  const auto &first = captures.back();
  // Trigger 2 opened it; 3 and 4 joined and filled the two-row tail.
  EXPECT_EQ(Field(first, "trigger_sequence").as_number(), 2.0);
  EXPECT_EQ(Field(first, "last_sequence").as_number(), 4.0);
  EXPECT_EQ(Field(first, "trigger_count").as_number(), 3.0);
  EXPECT_EQ(Field(first, "recorded_triggers").as_array().size(), 2U);
  EXPECT_EQ(Field(first, "unrecorded_trigger_count").as_number(), 1.0);
  EXPECT_EQ(Field(first, "state").as_string(), "closed");
  // The next error opened a new, overlapping capture after closure.
  const auto &second = captures.front();
  EXPECT_EQ(Field(second, "trigger_sequence").as_number(), 5.0);
  EXPECT_EQ(Field(second, "first_sequence").as_number(), 2.0);
  EXPECT_EQ(Field(second, "state").as_string(), "open");
  const auto retention = content_identity::canonical_json(store.Query(HistorianQuery{}).retention);
  // Five rows are protected once each even though rows 2..4 are shared.
  EXPECT_NE(retention.find("\"protected_count\":\"5\""), std::string::npos);
}

TEST(EventHistorian, IncidentResolutionKeepsItsConditionSeverityButIsNotAFailureTrigger) {
  TemporaryDirectory directory;
  FakeWriterClock clock;
  SqliteEventHistoryStore store(directory.path(), kStart, 100U, SmallCaptureLimits(),
                                clock.Clock());
  const auto incident = [](const std::uint64_t index, const std::string &phase) {
    return MakeHistorianEvent(
        "manyforge.composer", "client_incident_" + phase, phase == "resolved" ? "INFO" : "ERROR",
        static_cast<std::int64_t>(100U + index), "2026-09-24T12:00:00Z",
        CanonicalValue(CanonicalValue::Object{
            {"incident_kind", CanonicalValue("viewer_scene_unverified")},
            {"original_severity", CanonicalValue("ERROR")},
            {"phase", CanonicalValue(phase)},
            {"schema_version", CanonicalValue("manyforge.client_incident_event.v2")},
        }));
  };
  // A resolution alone opens nothing.
  ASSERT_EQ(store.Append(incident(1, "resolved")), IEventHistoryStore::AppendStatus::kStored);
  EXPECT_TRUE(CaptureList(store).empty());
  EXPECT_EQ(HistorianConditionSeverity(incident(1, "resolved")), "ERROR");
  // Activation opens a capture; the resolution enters its tail, not its triggers.
  ASSERT_EQ(store.Append(incident(2, "activated")), IEventHistoryStore::AppendStatus::kStored);
  ASSERT_EQ(store.Append(incident(3, "resolved")), IEventHistoryStore::AppendStatus::kStored);
  const auto captures = CaptureList(store);
  ASSERT_EQ(captures.size(), 1U);
  EXPECT_EQ(Field(captures.front(), "trigger_sequence").as_number(), 2.0);
  EXPECT_EQ(Field(captures.front(), "trigger_count").as_number(), 1.0);
  EXPECT_EQ(Field(captures.front(), "last_sequence").as_number(), 3.0);
}

TEST(EventHistorian, FullCaptureClosesBeforeARowThatDoesNotFitAndNeverBlocksTheWriter) {
  TemporaryDirectory directory;
  FakeWriterClock clock;
  HistorianCaptureLimits limits;
  limits.preceding_rows = 0U;
  limits.preceding_bytes = 1;
  limits.tail_rows = 1000U;
  limits.tail_duration = 1h;
  limits.capture_bytes = 600LL * 1024LL;
  limits.live_captures = 2U;
  limits.protected_bytes = limits.capture_bytes;  // Combined budget equal to one capture.
  SqliteEventHistoryStore store(directory.path(), kStart, 1000U, limits, clock.Clock());
  const auto large = [](const std::uint64_t index, const std::string &severity) {
    return MakeHistorianEvent("manyforge.runtime", "observation", severity,
                              static_cast<std::int64_t>(index), "2026-09-24T12:00:00Z",
                              CanonicalValue(CanonicalValue::Object{
                                  {"blob", CanonicalValue(std::string(100000U, 'x'))}}));
  };
  ASSERT_EQ(store.Append(large(1, "ERROR")), IEventHistoryStore::AppendStatus::kStored);
  for (std::uint64_t index = 2; index <= 8; ++index) {
    ASSERT_EQ(store.Append(large(index, "INFO")), IEventHistoryStore::AppendStatus::kStored);
  }
  auto captures = CaptureList(store);
  ASSERT_EQ(captures.size(), 1U);
  EXPECT_EQ(Field(captures.front(), "state").as_string(), "truncated");
  EXPECT_EQ(Field(captures.front(), "close_reason").as_string(), "capture_bytes");
  EXPECT_EQ(Field(captures.front(), "last_sequence").as_number(), 6.0);
  EXPECT_LE(std::stoll(Field(captures.front(), "capture_bytes").as_string()), limits.capture_bytes);
  // A failure after truncation opens the next capture; the budget retires the
  // older one instead of failing the append.
  ASSERT_EQ(store.Append(large(9, "ERROR")), IEventHistoryStore::AppendStatus::kStored);
  captures = CaptureList(store);
  ASSERT_EQ(captures.size(), 1U);
  EXPECT_EQ(Field(captures.front(), "trigger_sequence").as_number(), 9.0);
  const auto retention = content_identity::canonical_json(store.Query(HistorianQuery{}).retention);
  EXPECT_NE(retention.find("\"retired_count\":\"1\""), std::string::npos);
}

TEST(EventHistorian, LossAfterTheLastRowMakesAnExpiredCaptureUncertain) {
  TemporaryDirectory directory;
  FakeWriterClock clock;
  SqliteEventHistoryStore store(directory.path(), kStart, 100U, SmallCaptureLimits(),
                                clock.Clock());
  ASSERT_EQ(store.Append(ErrorEvent(1)), IEventHistoryStore::AppendStatus::kStored);
  store.ObserveDeliveryLoss(1U);  // Lost within the tail, after the last retained row.
  clock.now_ns += 2'000'000'000;
  const auto derived = CaptureList(store).front();
  EXPECT_EQ(Field(derived, "coverage").as_string(), "uncertain");
  ASSERT_EQ(store.Append(RoutineEvent(2)), IEventHistoryStore::AppendStatus::kStored);
  const auto recorded = CaptureList(store).front();
  EXPECT_TRUE(Field(recorded, "closure_recorded").as_bool());
  EXPECT_EQ(Field(recorded, "coverage").as_string(), "uncertain");
  EXPECT_EQ(Field(recorded, "writer_delivery_loss_during").as_number(), 1.0);
  bool loss_named = false;
  for (const auto &item : Field(recorded, "uncertainty").as_array()) {
    loss_named = loss_named || item.as_string() == "writer_delivery_loss_during_capture";
  }
  EXPECT_TRUE(loss_named);
}

TEST(EventHistorian, QuietTailClosesOnTheWriterWhileReadsNeverWriteCaptureState) {
  TemporaryDirectory directory;
  FakeWriterClock clock;
  const auto path = SqliteEventHistoryStore::DatabasePath(directory.path());
  SqliteEventHistoryStore store(directory.path(), kStart, 100U, SmallCaptureLimits(), clock.Clock());
  ASSERT_EQ(store.Append(ErrorEvent(1)), IEventHistoryStore::AppendStatus::kStored);
  clock.now_ns += 2'000'000'000;  // Past the one-second tail without any event.
  const auto derived = CaptureList(store).front();
  EXPECT_EQ(Field(derived, "state").as_string(), "closed");
  EXPECT_EQ(Field(derived, "close_reason").as_string(), "tail_duration");
  EXPECT_FALSE(Field(derived, "closure_recorded").as_bool());
  EXPECT_EQ(CaptureState(path, 1), "open");  // The query wrote nothing.
  ASSERT_EQ(store.Append(RoutineEvent(2)), IEventHistoryStore::AppendStatus::kStored);
  EXPECT_EQ(CaptureState(path, 1), "closed");
  const auto recorded = CaptureList(store).front();
  EXPECT_TRUE(Field(recorded, "closure_recorded").as_bool());
  // The late event was not admitted into the expired tail.
  EXPECT_EQ(Field(recorded, "last_sequence").as_number(), 1.0);
}

TEST(EventHistorian, CombinedCaptureBudgetRetiresTheOldestAndReturnsRowsToOrdinaryPruning) {
  TemporaryDirectory directory;
  FakeWriterClock clock;
  SqliteEventHistoryStore store(directory.path(), kStart, 3U, SmallCaptureLimits(),
                                clock.Clock());
  std::uint64_t index = 0;
  for (int capture = 0; capture < 3; ++capture) {
    for (int routine = 0; routine < 4; ++routine) ASSERT_EQ(store.Append(RoutineEvent(++index)), IEventHistoryStore::AppendStatus::kStored);
    ASSERT_EQ(store.Append(ErrorEvent(++index)), IEventHistoryStore::AppendStatus::kStored);
    for (int routine = 0; routine < 2; ++routine) ASSERT_EQ(store.Append(RoutineEvent(++index)), IEventHistoryStore::AppendStatus::kStored);
  }
  const auto captures = CaptureList(store);
  ASSERT_EQ(captures.size(), 2U);
  EXPECT_EQ(Field(captures.back(), "capture_id").as_string(), "2");
  const auto retention = Field(store.Query(HistorianQuery{}).retention, "captures");
  EXPECT_EQ(Field(retention, "retired_count").as_string(), "1");
  EXPECT_EQ(Field(retention, "last_retired_capture_id").as_string(), "1");
  EXPECT_EQ(Field(retention, "released_rows").as_string(), "6");
  // Released routine rows are ordinary rows again: the three-row routine cap
  // has already pruned them, while the retained captures stay intact.
  EXPECT_FALSE(store.Read(2U));
  HistorianQuery retired;
  retired.capture_id = 1U;
  ExpectHistorianError([&] { static_cast<void>(store.Query(retired)); }, "historian_capture_not_found");
  // Row 15 was ordinary routine history pruned before the third capture
  // opened; a capture never restores what was already pruned.
  EXPECT_FALSE(store.Read(15U));
  for (std::uint64_t sequence = 9; sequence <= 21; ++sequence) {
    if (sequence != 15U) {
      EXPECT_TRUE(store.Read(sequence)) << sequence;
    }
  }
}

TEST(EventHistorian, RestartClosesTheOpenTailAndValidatesDerivedProtection) {
  TemporaryDirectory directory;
  const auto path = SqliteEventHistoryStore::DatabasePath(directory.path());
  {
    FakeWriterClock clock;
    SqliteEventHistoryStore store(directory.path(), kStart, 100U, SmallCaptureLimits(), clock.Clock());
    ASSERT_EQ(store.Append(RoutineEvent(1)), IEventHistoryStore::AppendStatus::kStored);
    store.ObserveDeliveryLoss(2U);
    ASSERT_EQ(store.Append(ErrorEvent(2)), IEventHistoryStore::AppendStatus::kStored);
    store.ObserveDeliveryLoss(3U);
    ASSERT_EQ(store.Append(RoutineEvent(3)), IEventHistoryStore::AppendStatus::kStored);
  }
  {
    FakeWriterClock clock;
    SqliteEventHistoryStore restarted(directory.path(), kStart, 100U, SmallCaptureLimits(), clock.Clock());
    const auto capture = CaptureList(restarted).front();
    EXPECT_EQ(Field(capture, "state").as_string(), "closed_by_restart");
    EXPECT_EQ(Field(capture, "coverage").as_string(), "uncertain");
    const auto uncertainty = content_identity::canonical_json(Field(capture, "uncertainty"));
    EXPECT_NE(uncertainty.find("tail_interrupted_by_restart"), std::string::npos);
    EXPECT_NE(uncertainty.find("writer_delivery_loss_before_capture"), std::string::npos);
    EXPECT_NE(uncertainty.find("writer_delivery_loss_during_capture"), std::string::npos);
    // A new error in the next process whose context reaches rows of the
    // previous writer reports that boundary instead of claiming coverage.
    ASSERT_EQ(restarted.Append(ErrorEvent(4)), IEventHistoryStore::AppendStatus::kStored);
    const auto after = CaptureList(restarted).front();
    EXPECT_NE(content_identity::canonical_json(Field(after, "uncertainty")).find("context_spans_writer_restart"),
              std::string::npos);
  }
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, "UPDATE historian_events SET capture_refs=0 WHERE sequence=2", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  ExpectHistorianError([&] { SqliteEventHistoryStore rejected(directory.path(), kStart, 100U, SmallCaptureLimits()); },
                       "historian_storage_corrupt");
}

// Live finding: incident cause and closure codes were not
// search text, while the incident id was.
TEST(EventHistorian, IncidentCauseClosureAndVendorCodesAreSearchable) {
  TemporaryDirectory directory;
  const auto found = [](SqliteEventHistoryStore &store, const std::string &text) {
    HistorianQuery query;
    query.search = text;
    return store.Query(query).events.size();
  };
  for (const bool reopened : {false, true}) {
    SqliteEventHistoryStore store(directory.path(), kStart, 8U);
    if (!reopened) {
      ASSERT_EQ(store.Append(ComponentIncidentEvent(1, "provider_lost", false)), IEventHistoryStore::AppendStatus::kStored);
      ASSERT_EQ(store.Append(ComponentIncidentEvent(1, "provider_lost", true)), IEventHistoryStore::AppendStatus::kStored);
      ASSERT_EQ(store.Append(ComponentIncidentEvent(2, "emergency_stop", false)), IEventHistoryStore::AppendStatus::kStored);
    }
    EXPECT_EQ(found(store, "provider_lost"), 2U);
    EXPECT_EQ(found(store, "cell_closed_by_operator"), 1U);
    EXPECT_EQ(found(store, "emergency_stop"), 1U);
    EXPECT_EQ(found(store, "mode=0"), 3U);
    EXPECT_EQ(found(store, "state=2"), 1U);
    EXPECT_EQ(found(store, "incident.arm_robot.1"), 2U);
    EXPECT_EQ(found(store, "incident.arm_robot.2"), 1U);
    // Codes are search text only, never correlation discovery kinds.
    ExpectHistorianError([&] { (void)store.Discover("cause_class", "", 8U); }, "historian_query_invalid");
    for (const auto *text : {"summaryneedle", "providerneedle", "epochneedle", "component_incident_event.v2"})
      EXPECT_EQ(found(store, text), 0U) << text;
  }
}

TEST(EventHistorian, InconsistentCaptureLimitsAreRejected) {
  TemporaryDirectory directory;
  auto limits = SmallCaptureLimits();
  limits.protected_bytes = limits.capture_bytes - 1;
  ExpectHistorianError([&] { SqliteEventHistoryStore rejected(directory.path(), kStart, 100U, limits); },
                       "historian_configuration_invalid");
}

// ---------------------------------------------------------------------------
// The History file is named by its format; earlier files are never opened.

std::int64_t UserVersion(const std::filesystem::path &path) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    throw std::runtime_error("user_version read failed");
  }
  sqlite3_stmt *statement = nullptr;
  sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &statement, nullptr);
  const auto version = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return version;
}

TEST(EventHistorian, AnEmptyDirectoryCreatesTheFileNamedByItsFormat) {
  TemporaryDirectory directory;
  const auto path = SqliteEventHistoryStore::DatabasePath(directory.path());
  EXPECT_EQ(path.parent_path(), directory.path());
  {
    SqliteEventHistoryStore store(directory.path(), kStart);
    ASSERT_EQ(store.Append(Event(1U)), IEventHistoryStore::AppendStatus::kStored);
    EXPECT_TRUE(Field(store.Query(HistorianQuery{}).retention, "older_history_files").as_array().empty());
  }
  ASSERT_TRUE(std::filesystem::is_regular_file(path));
  EXPECT_EQ(path.filename().string(), "history.v" + std::to_string(UserVersion(path)) + ".sqlite3");
  SqliteEventHistoryStore reopened(directory.path(), kStart);
  EXPECT_TRUE(reopened.Read(1U));
}

TEST(EventHistorian, EarlierHistoryFilesAreListedButNeverOpened) {
  TemporaryDirectory directory;
  const auto name = SqliteEventHistoryStore::DatabasePath(directory.path()).filename().string();
  const auto format = std::stoi(name.substr(std::string("history.v").size()));
  ASSERT_GT(format, 1);
  // None of these is a SQLite database: opening one would fail the store.
  const std::string unreadable = "not a history database";
  for (const auto &file : {std::string("history.sqlite3"), std::string("history.sqlite3-wal"),
                           std::string("history.v1.sqlite3"), std::string("history.v01.sqlite3"),
                           "history.v" + std::to_string(format + 1) + ".sqlite3", std::string("other.sqlite3")}) {
    std::FILE *output = std::fopen((directory.path() / file).c_str(), "wb");
    ASSERT_NE(output, nullptr);
    std::fputs(unreadable.c_str(), output);
    std::fclose(output);
  }
  SqliteEventHistoryStore store(directory.path(), kStart);
  ASSERT_EQ(store.Append(Event(1U)), IEventHistoryStore::AppendStatus::kStored);
  const auto retention = store.Query(HistorianQuery{}).retention;
  EXPECT_EQ(Field(retention, "schema_version").as_string(), "manyforge.historian_retention.v3");
  const auto &older = Field(retention, "older_history_files").as_array();
  ASSERT_EQ(older.size(), 2U);
  EXPECT_EQ(Field(older[0], "file_name").as_string(), "history.sqlite3");
  EXPECT_EQ(content_identity::canonical_json(Field(older[0], "format")), "null");
  EXPECT_EQ(Field(older[1], "file_name").as_string(), "history.v1.sqlite3");
  EXPECT_EQ(Field(older[1], "format").as_string(), "1");
  std::FILE *input = std::fopen((directory.path() / "history.sqlite3").c_str(), "rb");
  ASSERT_NE(input, nullptr);
  std::string content(64U, '\0');
  content.resize(std::fread(content.data(), 1U, content.size(), input));
  std::fclose(input);
  EXPECT_EQ(content, unreadable);
}

TEST(EventHistorian, AFileOfTheCurrentNameHoldingAnotherFormatIsRefused) {
  TemporaryDirectory directory;
  const auto path = SqliteEventHistoryStore::DatabasePath(directory.path());
  { SqliteEventHistoryStore store(directory.path(), kStart); ASSERT_EQ(store.Append(Event(1U)), IEventHistoryStore::AppendStatus::kStored); }
  const auto format = UserVersion(path);
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  const auto older = "UPDATE historian_metadata SET schema_version=" + std::to_string(format - 1) +
                     "; PRAGMA user_version=" + std::to_string(format - 1) + ";";
  ASSERT_EQ(sqlite3_exec(database, older.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  ExpectHistorianError([&] { SqliteEventHistoryStore rejected(directory.path(), kStart); }, "historian_storage_foreign");
  // Refused, not converted: the file keeps its format.
  EXPECT_EQ(UserVersion(path), format - 1);
}

// ---------------------------------------------------------------------------
// Ids minted inside one application start are integers (contract 1.5).

// An operation event as Headless ingests it (operation_history_event.v2).
HistorianEvent OperationHistoryEvent(const std::string &start, double operation_id,
                                     const std::string &record_kind) {
  return MakeHistorianEvent(
      "manyforge.runtime", "operation_" + record_kind, "INFO", 100, "2026-11-13T00:00:00Z",
      CanonicalValue(CanonicalValue::Object{
          {"application_start_id", CanonicalValue(start)},
          {"journal_payload",
           CanonicalValue(CanonicalValue::Object{
               {"operation_state",
                CanonicalValue(CanonicalValue::Object{
                    {"operation_id", CanonicalValue(operation_id)},
                    {"program_run_id", CanonicalValue(3.0)},
                    {"participants",
                     CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                         {"execution_scope", CanonicalValue(CanonicalValue::Object{
                                                 {"node_instance_id", CanonicalValue("move")},
                                                 {"node_attempt_id", CanonicalValue(4.0)}})}})})},
                    {"stop_event_id", CanonicalValue(12.0)}})},
               {"record_kind", CanonicalValue(record_kind)}})},
          {"incident_id", CanonicalValue(5.0)},
          {"operation_id", CanonicalValue(operation_id)},
          {"record_kind", CanonicalValue(record_kind)},
          {"request_key", CanonicalValue("c0000000000000050000000000000001")},
          {"schema_version", CanonicalValue("manyforge.operation_history_event.v2")},
      }));
}

TEST(EventHistorian, IdFiltersFindIntegerIdsByTheirDecimalText) {
  TemporaryDirectory directory;
  const auto event = OperationHistoryEvent(kStart, 7.0, "accepted");
  for (const bool reopened : {false, true}) {
    // Reopening also validates the key index against the original.
    SqliteEventHistoryStore store(directory.path(), kStart);
    if (!reopened) {
      ASSERT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
    }
    for (const auto &[kind, value] : std::map<std::string, std::string>{
             {"operation_id", "7"}, {"program_run_id", "3"}, {"node_attempt_id", "4"}, {"stop_event_id", "12"}}) {
      HistorianQuery query;
      query.correlations = {{kind, value}};
      EXPECT_EQ(store.Query(query).events.size(), 1U) << kind;
      query.correlations = {{kind, "8"}};
      EXPECT_TRUE(store.Query(query).events.empty()) << kind;
    }
    // Only ids minted inside a start are keyed from integers.
    HistorianQuery other;
    other.correlations = {{"incident_id", "5"}};
    EXPECT_TRUE(store.Query(other).events.empty());
    const auto discovered = store.Discover("operation_id", "", 8U);
    ASSERT_EQ(discovered.find_object_member("values")->as_array().size(), 1U);
    EXPECT_EQ(discovered.find_object_member("values")->as_array().front().as_string(), "7");
    // The Kernel clock of the start the event names.
    EXPECT_EQ(store.Read(1U)->clock_owner, "kernel:" + kStart);
  }
}

TEST(EventHistorian, TheSameNumberUnderTwoStartIdentitiesIsToldApart) {
  TemporaryDirectory directory;
  const std::string other_start = "fedcba9876543210";
  const auto first = OperationHistoryEvent(kStart, 7.0, "accepted");
  const auto second = OperationHistoryEvent(other_start, 7.0, "accepted");
  {
    SqliteEventHistoryStore store(directory.path(), kStart);
    ASSERT_EQ(store.Append(first), IEventHistoryStore::AppendStatus::kStored);
  }
  {
    SqliteEventHistoryStore store(directory.path(), other_start);
    ASSERT_EQ(store.Append(second), IEventHistoryStore::AppendStatus::kStored);
    HistorianQuery query;
    query.correlations = {{"operation_id", "7"}};
    auto page = store.Query(query);
    ASSERT_EQ(page.events.size(), 1U);
    EXPECT_EQ(page.events.front().event.event_id, second.event_id);
    // Filters that name no start-minted id still span every start.
    EXPECT_EQ(store.Query(HistorianQuery{}).events.size(), 2U);
    query.correlations = {{"channel", "manyforge.runtime"}};
    EXPECT_EQ(store.Query(query).events.size(), 2U);
  }
  SqliteEventHistoryStore store(directory.path(), kStart);
  HistorianQuery query;
  query.correlations = {{"operation_id", "7"}, {"stop_event_id", "12"}};
  const auto page = store.Query(query);
  ASSERT_EQ(page.events.size(), 1U);
  EXPECT_EQ(page.events.front().event.event_id, first.event_id);
}

TEST(EventHistorian, AFormatSixFileIsListedAndNeverOpened) {
  TemporaryDirectory directory;
  ASSERT_EQ(SqliteEventHistoryStore::DatabasePath(directory.path()).filename().string(), "history.v8.sqlite3");
  const auto v6 = directory.path() / "history.v6.sqlite3";
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(v6.c_str(), &database), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, "CREATE TABLE historian_events(sequence INTEGER PRIMARY KEY); "
                                   "PRAGMA user_version=6;", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  const auto read = [&] {
    std::FILE *input = std::fopen(v6.c_str(), "rb");
    std::string content;
    for (int c = std::fgetc(input); c != EOF; c = std::fgetc(input)) content.push_back(static_cast<char>(c));
    std::fclose(input);
    return content;
  };
  const auto before = read();
  SqliteEventHistoryStore store(directory.path(), kStart);
  ASSERT_EQ(store.Append(Event(1U)), IEventHistoryStore::AppendStatus::kStored);
  const auto older = Field(store.Query(HistorianQuery{}).retention, "older_history_files").as_array();
  ASSERT_EQ(older.size(), 1U);
  EXPECT_EQ(Field(older[0], "file_name").as_string(), "history.v6.sqlite3");
  EXPECT_EQ(Field(older[0], "format").as_string(), "6");
  EXPECT_EQ(read(), before);
  EXPECT_FALSE(std::filesystem::exists(directory.path() / "history.v6.sqlite3-wal"));
  EXPECT_FALSE(std::filesystem::exists(directory.path() / "history.v6.sqlite3-shm"));
  EXPECT_EQ(UserVersion(SqliteEventHistoryStore::DatabasePath(directory.path())), 8);
}

// ---------------------------------------------------------------------------
// The application start is a filter of History (OD-198).

const std::string kEarlierStart = "fedcba9876543210";

HistorianEvent ActionSpanEvent(const std::string &start, const std::string &span) {
  return MakeHistorianEvent("manyforge.program", "node_started", "INFO", 200, "2026-11-13T00:00:01Z",
                            CanonicalValue(CanonicalValue::Object{
                                {"action_span_id", CanonicalValue(span)},
                                {"start", CanonicalValue(start)}}));
}

HistorianEvent RunEvent(double run) {
  return MakeHistorianEvent("manyforge.program", "program_run_started", "INFO", 300 + static_cast<int>(run),
                            "2026-11-13T00:00:02Z",
                            CanonicalValue(CanonicalValue::Object{{"program_run_id", CanonicalValue(run)}}));
}

CanonicalValue DescribedReport(const CanonicalValue &start, const char session = 'b') {
  return CanonicalValue(CanonicalValue::Object{
      {"application_start_id", start},
      {"schema_version", CanonicalValue("manyforge.workbench_previous_session.v2")},
      {"session_id", CanonicalValue(std::string(32U, session))},
      {"unfinished_operations", start.is_null() ? CanonicalValue(nullptr) : CanonicalValue(CanonicalValue::Array{
          CanonicalValue(CanonicalValue::Object{{"disposition", CanonicalValue("host_replaced")},
                                                {"operation_id", CanonicalValue(7.0)}})})},
  });
}

TEST(EventHistorian, TheStartFilterSelectsOneStartByNumberAndAsAWhole) {
  TemporaryDirectory directory;
  const auto earlier = OperationHistoryEvent(kEarlierStart, 7.0, "accepted");
  const auto current = OperationHistoryEvent(kStart, 7.0, "accepted");
  {
    SqliteEventHistoryStore store(directory.path(), kEarlierStart);
    ASSERT_EQ(store.Append(earlier), IEventHistoryStore::AppendStatus::kStored);
    ASSERT_EQ(store.Append(ActionSpanEvent(kEarlierStart, "4")), IEventHistoryStore::AppendStatus::kStored);
  }
  for (const bool reopened : {false, true}) {
    SqliteEventHistoryStore store(directory.path(), kStart);
    if (!reopened) {
      ASSERT_EQ(store.Append(current), IEventHistoryStore::AppendStatus::kStored);
      ASSERT_EQ(store.Append(ActionSpanEvent(kStart, "4")), IEventHistoryStore::AppendStatus::kStored);
    }
    // A number names the current start unless another start is named.
    HistorianQuery query;
    query.correlations = {{"operation_id", "7"}};
    auto page = store.Query(query);
    ASSERT_EQ(page.events.size(), 1U);
    EXPECT_EQ(page.events.front().event.event_id, current.event_id);
    EXPECT_EQ(page.events.front().application_start_id, kStart);
    query.application_start_id = kEarlierStart;
    page = store.Query(query);
    ASSERT_EQ(page.events.size(), 1U);
    EXPECT_EQ(page.events.front().event.event_id, earlier.event_id);
    EXPECT_EQ(page.events.front().application_start_id, kEarlierStart);
    // The action span follows the same start.
    HistorianQuery span;
    span.correlations = {{"action_span_id", "4"}};
    page = store.Query(span);
    ASSERT_EQ(page.events.size(), 1U);
    EXPECT_EQ(page.events.front().application_start_id, kStart);
    span.application_start_id = kEarlierStart;
    page = store.Query(span);
    ASSERT_EQ(page.events.size(), 1U);
    EXPECT_EQ(page.events.front().application_start_id, kEarlierStart);
    // A whole earlier start, without a number.
    HistorianQuery whole;
    whole.application_start_id = kEarlierStart;
    page = store.Query(whole);
    ASSERT_EQ(page.events.size(), 2U);
    for (const auto &row : page.events) EXPECT_EQ(row.application_start_id, kEarlierStart);
    // Other filters still span every retained start.
    EXPECT_EQ(store.Query(HistorianQuery{}).events.size(), 4U);
    // A well-formed start without rows answers an empty page, like any filter without matches.
    whole.application_start_id = "1111111111111111";
    EXPECT_TRUE(store.Query(whole).events.empty());
    whole.application_start_id = "not-a-start";
    ExpectHistorianError([&] { (void)store.Query(whole); }, "historian_query_invalid");
    // The removed filter name is no correlation any more.
    HistorianQuery removed;
    removed.correlations = {{"source_owner_epoch", kStart}};
    ExpectHistorianError([&] { (void)store.Query(removed); }, "historian_query_invalid");
  }
}

TEST(EventHistorian, RunDiscoveryListsOneStartInNumberOrder) {
  TemporaryDirectory directory;
  {
    SqliteEventHistoryStore store(directory.path(), kEarlierStart);
    ASSERT_EQ(store.Append(RunEvent(5.0)), IEventHistoryStore::AppendStatus::kStored);
  }
  SqliteEventHistoryStore store(directory.path(), kStart);
  for (const double run : {10.0, 2.0, 1.0}) {
    ASSERT_EQ(store.Append(RunEvent(run)), IEventHistoryStore::AppendStatus::kStored);
  }
  const auto values = [](const CanonicalValue &page) {
    std::vector<std::string> result;
    for (const auto &value : Field(page, "values").as_array()) result.push_back(value.as_string());
    return result;
  };
  // The current start by default, sorted by number (not "10" before "2").
  auto page = store.Discover("program_run_id", "", 2U);
  EXPECT_EQ(values(page), (std::vector<std::string>{"1", "2"}));
  EXPECT_EQ(Field(page, "next_after").as_string(), "2");
  page = store.Discover("program_run_id", "2", 2U);
  EXPECT_EQ(values(page), (std::vector<std::string>{"10"}));
  EXPECT_FALSE(std::get<bool>(Field(page, "truncated").storage()));
  // An earlier start's runs.
  EXPECT_EQ(values(store.Discover("program_run_id", "", 8U, kEarlierStart)), (std::vector<std::string>{"5"}));
  // Kinds that do not restart per start still span every start.
  EXPECT_EQ(values(store.Discover("event_type", "", 8U)), (std::vector<std::string>{"program_run_started"}));
  ExpectHistorianError([&] { (void)store.Discover("program_run_id", "", 8U, "XYZ"); }, "historian_query_invalid");
}

TEST(EventHistorian, StartDiscoveryListsStartsWithRowsNewestFirst) {
  TemporaryDirectory directory;
  // Written oldest first: "ffff..." before "0000...", so the order is by
  // first row, not by identity text.
  const std::vector<std::string> starts{"ffff000000000001", "0000000000000002", "8888000000000003"};
  for (const auto &start : starts) {
    SqliteEventHistoryStore store(directory.path(), start);
    ASSERT_EQ(store.Append(ActionSpanEvent(start, "1")), IEventHistoryStore::AppendStatus::kStored);
  }
  SqliteEventHistoryStore store(directory.path(), starts.back());
  const auto values = [](const CanonicalValue &page) {
    std::vector<std::string> result;
    for (const auto &value : Field(page, "values").as_array()) result.push_back(value.as_string());
    return result;
  };
  auto page = store.Discover("application_start_id", "", 2U);
  EXPECT_EQ(values(page), (std::vector<std::string>{starts[2], starts[1]}));
  EXPECT_TRUE(std::get<bool>(Field(page, "truncated").storage()));
  EXPECT_EQ(Field(page, "next_after").as_string(), starts[1]);
  page = store.Discover("application_start_id", starts[1], 2U);
  EXPECT_EQ(values(page), (std::vector<std::string>{starts[0]}));
  EXPECT_FALSE(std::get<bool>(Field(page, "truncated").storage()));
  ExpectHistorianError([&] { (void)store.Discover("application_start_id", "", 33U); }, "historian_query_invalid");
}

TEST(EventHistorian, PreviousSessionRowsAreFiledUnderTheStartTheyDescribe) {
  TemporaryDirectory directory;
  const auto operation = OperationHistoryEvent(kEarlierStart, 7.0, "accepted");
  {
    SqliteEventHistoryStore store(directory.path(), kEarlierStart);
    ASSERT_EQ(store.Append(operation), IEventHistoryStore::AppendStatus::kStored);
  }
  const auto report = MakeHistorianEvent("manyforge.workbench", "previous_session_report", "INFO", 0, "unknown",
                                         DescribedReport(CanonicalValue(kEarlierStart)));
  const auto alarm = MakeHistorianEvent(
      "manyforge.alarm", "operator_alarm_activated", "INFO", 400, "2026-11-13T00:00:03Z",
      CanonicalValue(CanonicalValue::Object{
          {"definition_id", CanonicalValue("manyforge.runtime.previous_session_unclean")},
          {"details", CanonicalValue(CanonicalValue::Object{
                          {"previous_session", DescribedReport(CanonicalValue(kEarlierStart))}})}}));
  const auto unknown = MakeHistorianEvent("manyforge.workbench", "previous_session_report", "INFO", 0, "unknown",
                                          DescribedReport(CanonicalValue(nullptr), 'c'));
  for (const bool reopened : {false, true}) {
    SqliteEventHistoryStore store(directory.path(), kStart);
    if (!reopened) {
      for (const auto *event : {&report, &alarm, &unknown}) {
        ASSERT_EQ(store.Append(*event), IEventHistoryStore::AppendStatus::kStored);
      }
    }
    // "Operation 7 of the previous start": its own row, the report and the alarm.
    HistorianQuery previous;
    previous.correlations = {{"operation_id", "7"}};
    previous.application_start_id = kEarlierStart;
    std::set<std::string> found;
    for (const auto &row : store.Query(previous).events) found.insert(row.event.event_id);
    EXPECT_EQ(found, (std::set<std::string>{operation.event_id, report.event_id, alarm.event_id}));
    // Not under the current start's operation 7.
    HistorianQuery current;
    current.correlations = {{"operation_id", "7"}};
    EXPECT_TRUE(store.Query(current).events.empty());
    // A report that names no start (no operations) stays with the writing start.
    current.correlations.clear();
    current.application_start_id = kStart;
    const auto rows = store.Query(current).events;
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.front().event.event_id, unknown.event_id);
  }
}

TEST(EventHistorian, PageAndSummaryRowsNameTheirStart) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path(), kStart);
  ASSERT_EQ(store.Append(Event(1U)), IEventHistoryStore::AppendStatus::kStored);
  HistorianQuery query;
  const auto summary = store.QuerySummaries(query);
  const auto &summary_rows = Field(summary, "events").as_array();
  ASSERT_EQ(summary_rows.size(), 1U);
  EXPECT_EQ(Field(summary_rows.front(), "application_start_id").as_string(), kStart);
  const auto page = HistorianPageValue(store.Query(query));
  const auto &page_rows = Field(page, "events").as_array();
  ASSERT_EQ(page_rows.size(), 1U);
  EXPECT_EQ(Field(page_rows.front(), "application_start_id").as_string(), kStart);
  EXPECT_EQ(store.Read(1U)->application_start_id, kStart);
}

} // namespace
} // namespace manyforge::headless
