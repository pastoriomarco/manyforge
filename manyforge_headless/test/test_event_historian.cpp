#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
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
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

#include "manyforge_headless/event_historian.hpp"

namespace manyforge::headless {
struct AsyncEventHistorianTestAccess {
  static void SetGeneration(AsyncEventHistorian &historian, std::uint64_t generation) {
    std::lock_guard lock(historian.mutex_);
    historian.health_generation_ = generation;
  }
};
namespace {

using content_identity::CanonicalValue;
using namespace std::chrono_literals;

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

constexpr const char *kRestoreLegacySchema = R"sql(
  DROP TABLE historian_search; DROP VIEW historian_search_content;
  DROP TABLE historian_event_keys; DROP TABLE historian_retention;
  DROP INDEX historian_events_utc_idx; DROP INDEX historian_events_clock_idx;
  DROP INDEX historian_events_severity_idx; DROP INDEX historian_events_retention_idx;
  DROP INDEX historian_events_coverage_idx;
  ALTER TABLE historian_events DROP COLUMN occurred_at_utc_ns;
  ALTER TABLE historian_events DROP COLUMN clock_owner;
  ALTER TABLE historian_events DROP COLUMN wall_clock_regressed;
  ALTER TABLE historian_events DROP COLUMN condition_severity;
  ALTER TABLE historian_events DROP COLUMN retention_class;
  ALTER TABLE historian_events DROP COLUMN record_bytes;
  ALTER TABLE historian_metadata DROP COLUMN prior_pruning_known;
  UPDATE historian_metadata SET schema_version=1; PRAGMA user_version=1;
)sql";

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

class ControllableStore final : public IEventHistoryStore {
public:
  AppendStatus Append(const HistorianEvent &event) override {
    std::unique_lock lock(mutex_);
    ++append_calls_;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return !blocked_; });
    if (failing_) {
      throw EventHistorianError("injected_storage_failure",
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

  void SetFailing(const bool failing) {
    std::lock_guard lock(mutex_);
    failing_ = failing;
    condition_.notify_all();
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

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool blocked_{};
  bool failing_{};
  std::size_t append_calls_{};
  std::map<std::string, std::string> events_;
};

TEST(EventHistorian, SqliteStoreIsIdempotentRetainedRestartableAndPaged) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "events.sqlite3";
  {
    SqliteEventHistoryStore store(path, 3U);
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

  SqliteEventHistoryStore reopened(path, 3U);
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
  SqliteEventHistoryStore store(directory.path() / "history.sqlite3", 128U);
  const auto payload = [](std::string clock, std::string message) {
    return CanonicalValue(CanonicalValue::Object{
        {"clock_owner", CanonicalValue(std::move(clock))}, {"message", CanonicalValue(std::move(message))},
        {"detail", CanonicalValue(CanonicalValue::Object{
          {"program_run_id", CanonicalValue("run.needle")}, {"action_span_id", CanonicalValue("action.needle")},
          {"branches", CanonicalValue(CanonicalValue::Array{
            CanonicalValue(CanonicalValue::Object{{"operation_id", CanonicalValue("op.first")}}),
            CanonicalValue(CanonicalValue::Object{{"operation_id", CanonicalValue("op.second")}})})}})},
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
  query.correlations = {{"program_run_id", "run.needle"}, {"operation_id", "op.second"}, {"action_span_id", "action.needle"}};
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

TEST(EventHistorian, ClockRegressionsAreNotConfusedWithDelayedOtherOwnersOrOldEvents) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path() / "history.sqlite3");
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
  const auto path = directory.path() / "history.sqlite3";
  {
    SqliteEventHistoryStore store(path);
    (void)store.Append(Event(1));
  }
  sqlite3 *raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw, "UPDATE sqlite_sequence SET seq=9007199254740990 WHERE name='historian_events'", nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_close(raw);
  SqliteEventHistoryStore store(path);
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
  EXPECT_NE(legacy_public.find("manyforge.historian_event.v2"),
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
  EXPECT_NE(public_value.find("manyforge.historian_event.v2"),
            std::string::npos);
  EXPECT_NE(public_value.find("\"occurred_at_ns\":\"9007199254740992\""),
            std::string::npos);

  TemporaryDirectory directory;
  const auto path = directory.path() / "long-service.sqlite3";
  {
    SqliteEventHistoryStore store(path, 2U);
    EXPECT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
  }
  SqliteEventHistoryStore reopened(path, 2U);
  const auto page = reopened.Query(HistorianQuery{});
  ASSERT_EQ(page.events.size(), 1U);
  EXPECT_EQ(page.events.front().event.occurred_at_ns, timestamp);
}

TEST(EventHistorian, ImportantRetentionSurvivesRoutineChurnAndReportsPrunedCoverage) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "protected.sqlite3";
  const auto important = MakeHistorianEvent("manyforge.composer", "client_incident_resolved", "INFO", 9,
      "2026-09-05T00:00:00Z", CanonicalValue(CanonicalValue::Object{
          {"schema_version", CanonicalValue("manyforge.client_incident_event.v1")},
          {"original_severity", CanonicalValue("ERROR")},
          {"message", CanonicalValue("retained gripper fault")},
      }));
  {
    SqliteEventHistoryStore store(path, 2U);
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
  SqliteEventHistoryStore restarted(path, 2U);
  EXPECT_TRUE(restarted.Read(1U));
  EXPECT_NE(content_identity::canonical_json(restarted.Query(HistorianQuery{}).retention)
      .find("\"pruned_count\":\"18\""), std::string::npos);
}

TEST(EventHistorian, PayloadByteRetentionIsBoundedIndependentlyOfRowCapacity) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path() / "byte-retention.sqlite3", 100000U);
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

TEST(EventHistorian, DerivedIndexTamperIsNotPresentedAsAnIntactRecord) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "index-tamper.sqlite3";
  { SqliteEventHistoryStore store(path, 2U); ASSERT_EQ(store.Append(Event(1)), IEventHistoryStore::AppendStatus::kStored); }
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, "UPDATE historian_events SET condition_severity='ERROR'", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  ExpectHistorianError([&] { SqliteEventHistoryStore store(path, 2U); }, "historian_storage_corrupt");
}

TEST(EventHistorian, SecondaryIndexTamperIsRejectedBeforeQueries) {
  TemporaryDirectory directory;
  for (const bool search : {false, true}) {
    const auto path = directory.path() / (search ? "search.sqlite3" : "correlation.sqlite3");
    { SqliteEventHistoryStore store(path, 8U); ASSERT_EQ(store.Append(Event(1)), IEventHistoryStore::AppendStatus::kStored); }
    sqlite3 *database = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    const auto sql = search
        ? "INSERT INTO historian_search(rowid,component,event_type,details) VALUES(99,'fake','fake','gripper phantom')"
        : "INSERT INTO historian_event_keys(sequence,kind,value) VALUES(1,'operation_id','operation.fake')";
    ASSERT_EQ(sqlite3_exec(database, sql, nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    ExpectHistorianError([&] { SqliteEventHistoryStore store(path, 8U); }, "historian_storage_corrupt");
  }
}

TEST(EventHistorian, DiagnosticLossCountersCannotAuthorizeDeletingRetainedEvidence) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "loss-counters.sqlite3";
  { SqliteEventHistoryStore store(path, 8U);
    ASSERT_EQ(store.Append(Event(1)), IEventHistoryStore::AppendStatus::kStored);
    ASSERT_EQ(store.Append(Event(2)), IEventHistoryStore::AppendStatus::kStored); }
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  // This is only diagnostic provenance. There is no persistent usage counter
  // whose corruption can tell the writer to delete an intact record.
  ASSERT_EQ(sqlite3_exec(database, "UPDATE historian_retention SET pruned_count=10", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  SqliteEventHistoryStore reopened(path, 8U);
  ASSERT_EQ(reopened.Append(Event(3)), IEventHistoryStore::AppendStatus::kStored);
  EXPECT_EQ(reopened.Query(HistorianQuery{}).events.size(), 3U);
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, "UPDATE historian_retention SET pruned_count=11", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  ExpectHistorianError([&] { static_cast<void>(reopened.Append(Event(4))); }, "historian_storage_changed");
}

TEST(EventHistorian, LegacyIndexMigrationPreservesBytesAndRollsBackCorruption) {
  TemporaryDirectory directory;
  for (const bool corrupt : {false, true}) {
    const auto path = directory.path() / (corrupt ? "corrupt-legacy.sqlite3" : "legacy.sqlite3");
    CanonicalValue deep(0.0);
    for (int depth = 0; depth < 63; ++depth) deep = CanonicalValue(CanonicalValue::Array{std::move(deep)});
    const auto event = MakeHistorianEvent("manyforge.runtime", "operation_progressed", "INFO", 1,
        "unknown legacy time", CanonicalValue(CanonicalValue::Object{
          {"operation_id", CanonicalValue("operation.1")},
          {"dense", CanonicalValue(CanonicalValue::Array(100000, CanonicalValue(0.0)))},
          {"deep", std::move(deep)},
        }));
    ASSERT_NO_THROW(static_cast<void>(deployment::parse_strict_json_model_yaml(
        content_identity::canonical_json(event.payload))));
    { SqliteEventHistoryStore store(path, 8U); ASSERT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored); }
    // Restore the exact committed v1 physical layout, keeping every original
    // payload byte, event identity and integrity commitment in place.
    sqlite3 *database = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(database, kRestoreLegacySchema, nullptr, nullptr, nullptr), SQLITE_OK);
    if (corrupt) { ASSERT_EQ(sqlite3_exec(database, "UPDATE historian_events SET payload_json='{}'", nullptr, nullptr, nullptr), SQLITE_OK); }
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    if (corrupt) {
      ExpectHistorianError([&] { SqliteEventHistoryStore rejected(path, 8U); }, "historian_storage_corrupt");
      ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
      sqlite3_stmt *version = nullptr;
      ASSERT_EQ(sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &version, nullptr), SQLITE_OK);
      ASSERT_EQ(sqlite3_step(version), SQLITE_ROW);
      EXPECT_EQ(sqlite3_column_int(version, 0), 1);
      sqlite3_finalize(version);
      ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    } else {
      SqliteEventHistoryStore upgraded(path, 8U);
      const auto migrated = upgraded.Read(1U);
      ASSERT_TRUE(migrated);
      EXPECT_EQ(migrated->event.event_id, event.event_id);
      EXPECT_EQ(content_identity::canonical_json(HistorianPersistenceValue(migrated->event)),
                content_identity::canonical_json(HistorianPersistenceValue(event)));
      EXPECT_FALSE(migrated->occurred_at_utc_ns);
      HistorianQuery query;
      query.correlations = {{"operation_id", "operation.1"}};
      EXPECT_EQ(upgraded.Query(query).events.size(), 1U);
    }
  }
}

TEST(EventHistorian, NearCapacityLegacyMigrationReusesPrunedPagesBeforeIndexing) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "near-capacity.sqlite3";
  { SqliteEventHistoryStore empty(path, 2000U); }
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, kRestoreLegacySchema, nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, "PRAGMA max_page_count=131072; BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_stmt *insert = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(database,
      "INSERT INTO historian_events(event_id,component,event_type,severity,occurred_at_ns,occurred_at_wall,payload_json,payload_digest,event_digest) "
      "VALUES(?,'manyforge.runtime','operation_progressed','INFO',?,'2026-09-06T12:00:00Z',?,?,?)", -1, &insert, nullptr), SQLITE_OK);
  const CanonicalValue payload(CanonicalValue::Object{{"blob", CanonicalValue(std::string(250000, 'x'))},
                                                      {"program_run_id", CanonicalValue("run.legacy")}});
  const auto payload_json = content_identity::canonical_json(payload);
  const auto payload_digest = content_identity::sha256_digest(payload_json);
  std::string last_identity;
  for (std::int64_t i = 1; i <= 2000; ++i) {
    const auto event = MakeHistorianEvent("manyforge.runtime", "operation_progressed", "INFO", i,
                                         "2026-09-06T12:00:00Z", payload);
    const auto digest = content_identity::sha256_digest(content_identity::canonical_json(HistorianPersistenceValue(event)));
    const auto bind_text = [&](int column, const std::string &value) {
      return sqlite3_bind_text(insert, column, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    };
    ASSERT_EQ(bind_text(1, event.event_id), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int64(insert, 2, i), SQLITE_OK);
    ASSERT_EQ(bind_text(3, payload_json), SQLITE_OK);
    ASSERT_EQ(bind_text(4, payload_digest), SQLITE_OK);
    ASSERT_EQ(bind_text(5, digest), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(insert), SQLITE_DONE);
    ASSERT_EQ(sqlite3_reset(insert), SQLITE_OK);
    last_identity = event.event_id;
  }
  sqlite3_finalize(insert);
  ASSERT_EQ(sqlite3_exec(database, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  EXPECT_GT(std::filesystem::file_size(path), 450U * 1024U * 1024U);
  EXPECT_LT(std::filesystem::file_size(path), 512U * 1024U * 1024U);
  SqliteEventHistoryStore upgraded(path, 2000U);
  const auto last = upgraded.Read(2000U);
  ASSERT_TRUE(last);
  EXPECT_EQ(last->event.event_id, last_identity);
  EXPECT_EQ(content_identity::canonical_json(last->event.payload), payload_json);
  EXPECT_FALSE(upgraded.Read(1U));
  HistorianQuery query;
  query.limit = 1;
  const auto page = upgraded.Query(query);
  const auto text = content_identity::canonical_json(page.retention);
  EXPECT_NE(text.find("\"pruned_count\":\"1933\""), std::string::npos);
  EXPECT_NE(text.find("\"prior_pruning_known\":false"), std::string::npos);
  EXPECT_LE(std::filesystem::file_size(path), 512U * 1024U * 1024U);
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
}

TEST(EventHistorian, DiscoveryAndExportUseBoundedExactContinuations) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path() / "export.sqlite3");
  for (int i = 0; i < 5; ++i) {
    (void)store.Append(MakeHistorianEvent("manyforge.runtime", "operation_progressed", "INFO", i,
        "2026-09-06T12:00:00Z", CanonicalValue(CanonicalValue::Object{
            {"blob", CanonicalValue(std::string(250000, 'x'))},
            {"program_run_id", CanonicalValue("run." + std::to_string(i))}})));
  }
  const auto discovery = store.Discover("program_run_id", "", 2);
  EXPECT_EQ(discovery.find_object_member("values")->as_array().size(), 2U);
  EXPECT_EQ(discovery.find_object_member("next_after")->as_string(), "run.1");
  EXPECT_EQ(store.Discover("program_run_id", "run.3", 2).find_object_member("values")->as_array().size(), 1U);
  ExpectHistorianError([&] { static_cast<void>(store.Discover("payload_json", "", 32)); }, "historian_query_invalid");
  HistorianQuery query;
  query.limit = 32;
  const auto exported = HistorianExportPageValue(store.Query(query));
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
  const auto path = directory.path() / "discovery.sqlite3";
  for (const bool reopened : {false, true}) {
    SqliteEventHistoryStore store(path);
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
  SqliteEventHistoryStore store(directory.path() / "channels.sqlite3");
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
  const auto path = directory.path() / "busy.sqlite3";
  {
    SqliteEventHistoryStore store(path, 2U);
    sqlite3 *blocker = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &blocker), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
    ExpectHistorianError([&] { static_cast<void>(store.Append(Event(1))); }, "historian_storage_busy");
    ASSERT_EQ(sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(blocker), SQLITE_OK);
    EXPECT_EQ(store.Append(Event(1)), IEventHistoryStore::AppendStatus::kStored);
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_read);
  ExpectHistorianError([&] { SqliteEventHistoryStore readonly(path, 2U); }, "historian_storage_failure");
  std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  SqliteEventHistoryStore recovered(path, 2U);
  EXPECT_TRUE(recovered.Read(1U));
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
               {"operation_id", CanonicalValue("operation.9")},
           })},
          {"phase", CanonicalValue("resolved")},
          {"program_run_id", CanonicalValue("program-run.7")},
      }));
  const auto encoded =
      content_identity::canonical_json(HistorianSummaryPageValue(
          HistorianPage{{HistorianStoredEvent{7U, event}}, true, 6U}));

  EXPECT_NE(encoded.find("manyforge.historian_summary_page.v2"),
            std::string::npos);
  EXPECT_NE(encoded.find("manyforge.historian_event_summary.v2"),
            std::string::npos);
  EXPECT_NE(encoded.find("Control delayed resolved"), std::string::npos);
  EXPECT_NE(encoded.find("program-run.7"), std::string::npos);
  EXPECT_NE(encoded.find("operation.9"), std::string::npos);
  EXPECT_NE(encoded.find("\"occurred_at_ns\":\"9007199254740992\""),
            std::string::npos);
  EXPECT_EQ(encoded.find("diagnostic_blob"), std::string::npos);
  EXPECT_EQ(encoded.find(std::string(1024U, 'x')), std::string::npos);
}

TEST(EventHistorian, DetailPublishesOnlyTypedSourceBackedSections) {
  const auto event = MakeHistorianEvent(
      "manyforge.execution", "operation_updated", "ERROR", 101,
      "2026-08-05T00:00:00Z",
      CanonicalValue(CanonicalValue::Object{
          {"blackboard", CanonicalValue(CanonicalValue::Object{
                             {"part_ready", CanonicalValue(true)},
                         })},
          {"duration_ns", CanonicalValue(1250.0)},
          {"operation_id", CanonicalValue("operation.1")},
          {"record_kind", CanonicalValue("observation")},
          {"schema_version",
           CanonicalValue("manyforge.operation_history_event.v1")},
      }));
  const auto encoded = content_identity::canonical_json(
      HistorianDetailValue(HistorianStoredEvent{7U, event}));
  EXPECT_NE(encoded.find("manyforge.historian_detail.v2"), std::string::npos);
  EXPECT_NE(encoded.find("operation.1"), std::string::npos);
  EXPECT_NE(encoded.find("part_ready"), std::string::npos);
  EXPECT_NE(encoded.find("duration_ns"), std::string::npos);
  EXPECT_NE(encoded.find("incident"), std::string::npos);
}

TEST(EventHistorian, SqliteStoreRejectsAliasForeignIdentityAndTamper) {
  TemporaryDirectory directory;
  const auto target = directory.path() / "target.sqlite3";
  {
    SqliteEventHistoryStore store(target, 8U);
    EXPECT_EQ(store.Append(Event(1U)),
              IEventHistoryStore::AppendStatus::kStored);
  }
  const auto alias = directory.path() / "alias.sqlite3";
  std::filesystem::create_symlink(target.filename(), alias);
  ExpectHistorianError([&]() { SqliteEventHistoryStore store(alias, 8U); },
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
      [&]() { SqliteEventHistoryStore tampered(target, 8U); },
      "historian_storage_corrupt");

  const auto foreign = directory.path() / "foreign.sqlite3";
  ASSERT_EQ(sqlite3_open_v2(foreign.string().c_str(), &database,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, "CREATE TABLE foreign_table(value TEXT)",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  ExpectHistorianError([&]() { SqliteEventHistoryStore store(foreign, 8U); },
                       "historian_storage_foreign");
}

TEST(EventHistorian,
     AsyncQueueNeverWaitsForStorageAndOverflowNeedsExplicitRecovery) {
  auto store = std::make_shared<ControllableStore>();
  store->SetBlocked(true);
  AsyncEventHistorian historian(store, 2U);
  EXPECT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(store->WaitForAppendCalls(1U, 2s));

  EXPECT_TRUE(historian.TryEnqueue(Event(2U)));
  EXPECT_FALSE(historian.TryEnqueue(Event(3U)));
  auto health = historian.Health();
  EXPECT_FALSE(health.available);
  EXPECT_EQ(health.error_code, "historian_queue_overflow");
  EXPECT_EQ(health.queue_depth, 2U);
  EXPECT_EQ(health.overflow_count, 1U);

  store->SetBlocked(false);
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  health = historian.Health();
  EXPECT_FALSE(health.available);
  EXPECT_TRUE(health.recovery_ready);
  EXPECT_EQ(store->StoredCount(), 2U);
  EXPECT_TRUE(historian.ReconcileHealth(health.health_generation));
  EXPECT_TRUE(historian.Health().available);
  EXPECT_THROW(static_cast<void>(historian.ReconcileHealth(health.health_generation)),
               EventHistorianError);
  const auto reconciled_generation = historian.Health().health_generation;
  EXPECT_FALSE(historian.ReconcileHealth(reconciled_generation));
  EXPECT_EQ(historian.Health().health_generation, reconciled_generation);
}

TEST(EventHistorian,
     FailedWriteIsRetriedWithoutLosingFactAndCannotSelfReconcile) {
  auto store = std::make_shared<ControllableStore>();
  store->SetFailing(true);
  AsyncEventHistorian historian(store, 4U);
  EXPECT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(store->WaitForAppendCalls(1U, 2s));

  AsyncHistorianHealth failed;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    failed = historian.Health();
    std::this_thread::yield();
  } while (failed.available && std::chrono::steady_clock::now() < deadline);
  ASSERT_FALSE(failed.available);
  EXPECT_FALSE(failed.recovery_ready);
  EXPECT_FALSE(historian.ReconcileHealth(failed.health_generation));

  store->SetFailing(false);
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  const auto recovered_write = historian.Health();
  EXPECT_FALSE(recovered_write.available);
  EXPECT_TRUE(recovered_write.recovery_ready);
  EXPECT_EQ(store->StoredCount(), 1U);
  EXPECT_TRUE(historian.ReconcileHealth(recovered_write.health_generation));
  EXPECT_TRUE(historian.Health().available);

  EXPECT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  EXPECT_EQ(historian.Health().duplicate_count, 1U);
}

TEST(EventHistorian, ProjectionFailureUsesTheSameWriteAndGenerationRecoveryFence) {
  auto store = std::make_shared<ControllableStore>();
  AsyncEventHistorian historian(store, 2U);
  historian.ReportDeliveryFailure("historian_projection_failure", "payload too large");
  const auto failure = historian.Health();
  ASSERT_FALSE(failure.available);
  EXPECT_FALSE(historian.ReconcileHealth(failure.health_generation));
  ASSERT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  EXPECT_TRUE(historian.ReconcileHealth(failure.health_generation));
  historian.ReportDeliveryFailure("historian_projection_failure", "payload too large");
  EXPECT_THROW(static_cast<void>(historian.ReconcileHealth(failure.health_generation)),
               EventHistorianError);
  EXPECT_FALSE(historian.Health().available);
}

TEST(EventHistorian, HealthGenerationNeverWrapsOrAuthorizesRepairAtExhaustion) {
  auto store = std::make_shared<ControllableStore>();
  AsyncEventHistorian historian(store, 2U);
  constexpr std::uint64_t maximum = 9007199254740991ULL;
  AsyncEventHistorianTestAccess::SetGeneration(historian, maximum - 2);
  historian.ReportDeliveryFailure("injected", "first episode");
  EXPECT_EQ(historian.Health().health_generation, maximum - 1);
  ASSERT_TRUE(historian.TryEnqueue(Event(1U)));
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  EXPECT_TRUE(historian.ReconcileHealth(maximum - 1));
  EXPECT_EQ(historian.Health().health_generation, maximum);
  historian.ReportDeliveryFailure("injected", "second episode");
  ASSERT_TRUE(historian.TryEnqueue(Event(2U)));
  ASSERT_TRUE(historian.WaitUntilIdle(2s));
  EXPECT_FALSE(historian.ReconcileHealth(maximum));
  EXPECT_FALSE(historian.Health().available);
  EXPECT_FALSE(historian.Health().recovery_ready);
  EXPECT_EQ(historian.Health().health_generation, maximum);
  EXPECT_EQ(historian.Health().error_code, "historian_health_generation_exhausted");
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
      AlarmCallerAttribution{"request.1", "api.epoch.1", "operator_ui",
                             "correlation.1", "test", "manyforge.test.alarm",
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

} // namespace
} // namespace manyforge::headless
