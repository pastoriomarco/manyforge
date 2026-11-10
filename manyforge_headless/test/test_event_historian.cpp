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
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <exception>
#include <span>
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

HistorianEvent OperationFailureEvent(const std::string &message) {
  auto event = MakeHistorianEvent(
      "manyforge.runtime", "motion_retry_selected", "ERROR", 101,
      "2026-08-05T00:00:00Z",
      CanonicalValue(CanonicalValue::Object{
          {"schema_version",
           CanonicalValue("manyforge.operation_history_event.v1")},
          {"journal_payload",
           CanonicalValue(CanonicalValue::Object{
               {"operation_state",
                CanonicalValue(CanonicalValue::Object{
                    {"operation_id", CanonicalValue("operation.1")},
                    {"program_run_id", CanonicalValue("run.1")},
                    {"participants",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue(CanonicalValue::Object{
                             {"execution_scope",
                              CanonicalValue(CanonicalValue::Object{
                                  {"node_instance_id", CanonicalValue("move")},
                                  {"node_attempt_id",
                                   CanonicalValue("move.attempt.1")}})},
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

// Revisions 2-4 indexed the whole payload JSON and keyed no codes.
#define MANYFORGE_RESTORE_PAYLOAD_SEARCH_INDEX \
  "DROP TABLE historian_search; DROP VIEW historian_search_content;" \
  "CREATE VIEW historian_search_content AS " \
  "SELECT sequence,component,event_type,payload_json AS details FROM historian_events;" \
  "CREATE VIRTUAL TABLE historian_search USING fts5(component,event_type,details," \
  "content='historian_search_content',content_rowid='sequence',tokenize='trigram');" \
  "INSERT INTO historian_search(historian_search) VALUES('rebuild');" \
  "DELETE FROM historian_event_keys WHERE kind IN ('code','reason','reason_code','cause_class','closure_reason');"
constexpr const char* kRestorePayloadSearchIndex = MANYFORGE_RESTORE_PAYLOAD_SEARCH_INDEX;

// Revision 5 indexed the same human text without the incident cause, closure
// and vendor codes (no `cause_class`/`closure_reason` keys, no
// `details.vendor_codes` path).
const std::string kRestoreRevision5SearchIndex = [] {
  std::string details;
  for (const auto *path : {"$.message", "$.title", "$.incident_kind", "$.definition_id", "$.source_label",
                           "$.subject.subject_id", "$.subject.resource_ids", "$.details.reason", "$.details.reasons"}) {
    details += std::string("ifnull(CASE WHEN json_type(e.payload_json,'") + path + "') IN ('text','array') "
               "THEN json_extract(e.payload_json,'" + path + "')||char(10) END,'')||";
  }
  return "DROP TABLE historian_search; DROP VIEW historian_search_content;"
         "DELETE FROM historian_event_keys WHERE kind IN ('cause_class','closure_reason');"
         "CREATE VIEW historian_search_content AS SELECT e.sequence,e.component,e.event_type," + details +
         "ifnull((SELECT group_concat(k.value,char(10)) FROM historian_event_keys k WHERE k.sequence=e.sequence),'') "
         "AS details FROM historian_events e;"
         "CREATE VIRTUAL TABLE historian_search USING fts5(component,event_type,details,"
         "content='historian_search_content',content_rowid='sequence',tokenize='trigram');"
         "INSERT INTO historian_search(historian_search) VALUES('rebuild');"
         "UPDATE historian_metadata SET schema_version=5; PRAGMA user_version=5;";
}();

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

// Storage revision 4 adds only derived failure-capture protection.
constexpr const char* kRestoreRevision3Schema = MANYFORGE_RESTORE_PAYLOAD_SEARCH_INDEX R"sql(
  DROP TABLE historian_capture_triggers; DROP TABLE historian_captures;
  DROP TABLE historian_capture_retention; DROP INDEX historian_events_prune_idx;
  ALTER TABLE historian_events DROP COLUMN capture_refs;
  UPDATE historian_metadata SET schema_version=3; PRAGMA user_version=3;
)sql";

constexpr const char* kRestoreLegacySchema = R"sql(
  DROP TABLE historian_capture_triggers; DROP TABLE historian_captures;
  DROP TABLE historian_capture_retention; DROP INDEX historian_events_prune_idx;
  ALTER TABLE historian_events DROP COLUMN capture_refs;
  ALTER TABLE historian_events DROP COLUMN summary_json;
  ALTER TABLE historian_events DROP COLUMN summary_digest;
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
    if (failing_) {
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
  std::string failure_code_{"injected_storage_failure"};
  std::size_t append_calls_{};
  std::vector<std::size_t> batch_sizes_;
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

TEST(EventHistorian, ProgramDiagnosticValuesAreNeitherSearchTextNorOwnerIdentities) {
  TemporaryDirectory directory;
  // Originals recorded before the v3 cutover keep their version and derivation.
  for (const auto version : kProgramDiagnosticSchemaVersions) {
    const auto path = directory.path() / (std::string(version) + ".sqlite3");
    CanonicalValue::Object payload{
        {"schema_version", CanonicalValue(std::string(version))},
        {"program_run_id", CanonicalValue("run.actual")},
        {"blackboard", CanonicalValue(CanonicalValue::Object{
                           {"operation_id", CanonicalValue("operation.foreign")},
                           {"program_run_id", CanonicalValue("run.foreign")}})},
    };
    if (version == "manyforge.program_diagnostic.v1" ||
        version == "manyforge.program_diagnostic.v2") {
      // Retained v1/v2 rows also carry the withdrawn runtime-override section.
      payload.emplace_back("runtime_overrides",
                           CanonicalValue(CanonicalValue::Object{
                               {"operation_id", CanonicalValue("operation.foreign")}}));
    }
    const auto event = MakeHistorianEvent(
        "manyforge.program_runner", "program_failed", "ERROR", 100, "2026-09-08T00:00:00Z",
        CanonicalValue(std::move(payload)));
    for (const bool reopened : {false, true}) {
      SqliteEventHistoryStore store(path, 128U);
      if (!reopened) {
        EXPECT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
      }
      HistorianQuery query;
      query.correlations = {{"program_run_id", "run.actual"}};
      ASSERT_EQ(store.Query(query).events.size(), 1U);
      const auto summary_page = store.QuerySummaries(query);
      const auto& summary = *summary_page.find_object_member("events")
                                 ->as_array()
                                 .front()
                                 .find_object_member("event");
      EXPECT_EQ(summary.find_object_member("program_run_id")->as_string(), "run.actual");
      EXPECT_EQ(content_identity::canonical_json(*summary.find_object_member("operation_id")),
                "null");
      query.correlations = {{"program_run_id", "run.foreign"}};
      EXPECT_TRUE(store.Query(query).events.empty());
      query.correlations = {{"operation_id", "operation.foreign"}};
      EXPECT_TRUE(store.Query(query).events.empty());
      query.correlations.clear();
      // Search covers the producer identities, not user values in the payload.
      query.search = "run.foreign";
      EXPECT_TRUE(store.Query(query).events.empty());
      query.search = "run.actual";
      EXPECT_EQ(store.Query(query).events.size(), 1U);
    }
  }
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
    SqliteEventHistoryStore store(path, 2U, TriggerOnlyCaptureLimits());
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
  SqliteEventHistoryStore restarted(path, 2U, TriggerOnlyCaptureLimits());
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

TEST(EventHistorian, IndexedSearchReflectsPruningAfterRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "search-retention.sqlite3";
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
    SqliteEventHistoryStore store(path, 2U);
    ASSERT_EQ(store.Append(event(1, "retiredneedle")), IEventHistoryStore::AppendStatus::kStored);
    ASSERT_EQ(store.Append(event(2, "other")), IEventHistoryStore::AppendStatus::kStored);
    ASSERT_EQ(store.Append(event(3, "retainedneedle")), IEventHistoryStore::AppendStatus::kStored);
    EXPECT_FALSE(store.Read(1U));
    check_search(store);
  }
  SqliteEventHistoryStore reopened(path, 2U);
  check_search(reopened);
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
      query.correlations.clear();
      query.search = "operation.1";
      EXPECT_EQ(upgraded.Query(query).events.size(), 1U);
    }
  }
}

TEST(EventHistorian, SearchCoversHumanTextAndIdentitiesButNeverPayloadJson) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "search-text.sqlite3";
  const auto event = MakeHistorianEvent(
      "manyforge.runtime", "stop_terminal_projected", "WARN", 1, "2026-10-28T00:00:00Z",
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
                                  {"stop_id", CanonicalValue("stop.42")},
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
    SqliteEventHistoryStore store(path, 8U);
    if (!reopened) {
      ASSERT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
    }
    for (const auto *text : {"manyforge.runt", "terminal_proj", "closed on \"part\"", "grip_lost", "finger_blocked",
                             "sensor_stale", "cell.alpha", "arm.res1", "stop.42", "ik_failed",
                             // Array elements: literal quote and backslash.
                             "quoted \"jaw\" reason", "arm\\joint2"})
      EXPECT_EQ(found(store, text), 1U) << text;
    for (const auto *text : {"hiddenneedle", "nestedneedle", "sha256:abc", "payload_digest", "journal_payload"})
      EXPECT_EQ(found(store, text), 0U) << text;
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
  query.search = "run.legacy";
  const auto searched = upgraded.Query(query);
  ASSERT_EQ(searched.events.size(), 1U);
  EXPECT_EQ(searched.events.front().event.event_id, last_identity);
  EXPECT_LE(std::filesystem::file_size(path), 512U * 1024U * 1024U);
  // The migration's write-ahead log is reclaimed before the store is used.
  const auto wal = std::filesystem::path(path.string() + "-wal");
  EXPECT_TRUE(!std::filesystem::exists(wal) || std::filesystem::file_size(wal) == 0U);
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

TEST(EventHistorian, SqlitePageLimitReportsStorageFullAndKeepsCommittedEvents) {
  TemporaryDirectory directory;
  ScopedTestDatabaseCapture capture;
  ASSERT_EQ(capture.result(), SQLITE_OK);
  SqliteEventHistoryStore store(directory.path() / "full.sqlite3", 100U);
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
  SqliteEventHistoryStore store(directory.path() / "batch.sqlite3", 100U);
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
  SqliteEventHistoryStore store(directory.path() / "batch-bytes.sqlite3", 100000U);
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
  const auto path = directory.path() / "pinned.sqlite3";
  const auto wal = std::filesystem::path(path.string() + "-wal");
  const auto large = [](const std::int64_t index) {
    return MakeHistorianEvent("manyforge.runtime", "observation", "INFO", index,
                              "2026-09-06T12:00:00Z",
                              CanonicalValue(CanonicalValue::Object{
                                  {"blob", CanonicalValue(std::string(200000U, 'x'))}}));
  };
  SqliteEventHistoryStore store(path, 100000U);
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

TEST(EventHistorian,
     OperationFailureSummaryUsesOriginalErrorAndAttemptWithoutExtraRows) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path() / "operation-history.sqlite3");
  const auto event = OperationFailureEvent("No path from authored target");
  EXPECT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
  const auto summary =
      HistorianSummaryEventValue(event, std::nullopt, std::nullopt, false);
  EXPECT_EQ(
      summary.find_object_member("message")->as_string(),
      "[move / move.attempt.1] planning_failed: No path from authored target");
  EXPECT_EQ(summary.find_object_member("severity")->as_string(), "ERROR");
  EXPECT_EQ(summary.find_object_member("operation_id")->as_string(),
            "operation.1");
  EXPECT_EQ(summary.find_object_member("program_run_id")->as_string(), "run.1");
  const auto page = store.Query({});
  ASSERT_EQ(page.events.size(), 1U);
  EXPECT_EQ(page.events.front().event.payload, event.payload);
  const auto detail = content_identity::canonical_json(
      HistorianDetailValue(page.events.front()));
  EXPECT_NE(detail.find("planning_failed"), std::string::npos);
  EXPECT_NE(detail.find("node_attempt_id"), std::string::npos);
  EXPECT_NE(detail.find("move.attempt.1"), std::string::npos);
}

TEST(EventHistorian,
     OperationFailureSummaryIsBoundedAndPreservesLegacyProjection) {
  const auto long_event = OperationFailureEvent(std::string(9000U, 'x'));
  const auto summary =
      HistorianSummaryEventValue(long_event, std::nullopt, std::nullopt, false);
  const auto &message = summary.find_object_member("message")->as_string();
  EXPECT_EQ(message.size(), 1024U);
  EXPECT_EQ(message.find("[move / move.attempt.1] planning_failed: "), 0U);
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
  const auto path = directory.path() / "legacy-operation-history.sqlite3";
  {
    SqliteEventHistoryStore store(path);
    EXPECT_EQ(store.Append(legacy), IEventHistoryStore::AppendStatus::kStored);
  }
  SqliteEventHistoryStore reopened(path);
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
             {"node_attempt_id",
              CanonicalValue(std::string(node) + ".attempt.1")}})},
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
            "[peer / peer.attempt.1] cancelled: Peer stopped; "
            "[origin / origin.attempt.1] runtime_collision_fault: Collision "
            "detected");
  EXPECT_TRUE(OperationFailureHistoryMessage(CanonicalValue(nullptr)).empty());
}

TEST(EventHistorian, StoredSummariesMatchFullProjectionAcrossFiltersAndPages) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path() / "summaries.sqlite3", 2U);
  for (int i = 1; i <= 6; ++i) {
    (void)store.Append(MakeHistorianEvent(
        "manyforge.test", "connection_changed", i % 2 ? "ERROR" : "INFO", i,
        i == 6 ? "2026-09-08T00:00:00Z" : "2026-09-08T00:00:01Z",
        CanonicalValue(CanonicalValue::Object{{"clock_owner", CanonicalValue("owner.1")},
                                              {"program_run_id", CanonicalValue("run.actual")},
                                              {"message", CanonicalValue("searchable message")}})));
  }
  HistorianQuery query;
  query.limit = 1;
  for (int filter = 0; filter < 7; ++filter) {
    if (filter == 1) query.components = {"manyforge."};
    if (filter == 2) query.event_types = {"connection_changed"};
    if (filter == 3) query.correlations = {{"program_run_id", "run.actual"}};
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

TEST(EventHistorian, SummaryMigrationPreservesEscapedValuesLossAndStickyClockFacts) {
  TemporaryDirectory directory;
  for (const bool corrupt : {false, true}) {
    const auto path = directory.path() / (corrupt ? "corrupt-v2.sqlite3" : "v2.sqlite3");
    std::string expected, original;
    {
      SqliteEventHistoryStore store(path, 1U);
      for (int i = 1; i <= 2; ++i) {
        const std::string escaped(1024, '\1');
        // WARN keeps this migration test about ordinary pruning: an ERROR
        // would open a failure capture that protects the pruned row.
        const auto event = MakeHistorianEvent(
            escaped, escaped, "WARN", i, i == 1 ? "2026-09-08T00:00:01Z" : "2026-09-08T00:00:00Z",
            CanonicalValue(CanonicalValue::Object{{"clock_owner", CanonicalValue(escaped)},
                                                  {"program_run_id", CanonicalValue(escaped)},
                                                  {"operation_id", CanonicalValue(escaped)},
                                                  {"condition_id", CanonicalValue(escaped)},
                                                  {"message", CanonicalValue(escaped)}}));
        ASSERT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
        original = content_identity::canonical_json(HistorianPersistenceValue(event));
      }
      expected = content_identity::canonical_json(store.QuerySummaries(HistorianQuery{}));
      EXPECT_GT(expected.size(), 40000U);
      EXPECT_NE(expected.find("\"wall_clock_regressed\":true"), std::string::npos);
      EXPECT_NE(expected.find("\"pruned_count\":\"1\""), std::string::npos);
    }
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(database,
                           MANYFORGE_RESTORE_PAYLOAD_SEARCH_INDEX
                           "DROP TABLE historian_capture_triggers; DROP TABLE historian_captures;"
                           "DROP TABLE historian_capture_retention; DROP INDEX historian_events_prune_idx;"
                           "ALTER TABLE historian_events DROP COLUMN capture_refs;"
                           "ALTER TABLE historian_events DROP COLUMN summary_json;"
                           "ALTER TABLE historian_events DROP COLUMN summary_digest;"
                           "UPDATE historian_metadata SET schema_version=2; PRAGMA "
                           "user_version=2;",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    if (corrupt) {
      ASSERT_EQ(sqlite3_exec(database, "UPDATE historian_events SET payload_json='{}'", nullptr,
                             nullptr, nullptr),
                SQLITE_OK);
    }
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    if (!corrupt) {
      SqliteEventHistoryStore migrated(path, 1U);
      EXPECT_EQ(content_identity::canonical_json(migrated.QuerySummaries(HistorianQuery{})),
                expected);
      EXPECT_EQ(
          content_identity::canonical_json(HistorianPersistenceValue(migrated.Read(2)->event)),
          original);
    } else {
      ExpectHistorianError([&] { SqliteEventHistoryStore rejected(path, 1U); },
                           "historian_storage_corrupt");
      ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
      sqlite3_stmt* version = nullptr;
      ASSERT_EQ(sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &version, nullptr),
                SQLITE_OK);
      ASSERT_EQ(sqlite3_step(version), SQLITE_ROW);
      EXPECT_EQ(sqlite3_column_int(version, 0), 2);
      sqlite3_finalize(version);
      ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    }
  }
}

TEST(EventHistorian, TamperedSummaryCannotReplaceAnOriginalEvenWithARecomputedDigest) {
  TemporaryDirectory directory;
  for (const bool rehashed : {false, true}) {
    const auto path = directory.path() / (rehashed ? "rehashed.sqlite3" : "tampered.sqlite3");
    const auto event = Event(1);
    {
      SqliteEventHistoryStore store(path);
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
    ExpectHistorianError([&] { SqliteEventHistoryStore rejected(path); },
                         "historian_storage_corrupt");
  }
}

TEST(EventHistorian, V2DiagnosticIndexMigratesOnlyAfterItsHistoricalDerivationIsVerified) {
  TemporaryDirectory directory;
  for (const bool corrupt : {false, true}) {
    const auto path =
        directory.path() / (corrupt ? "corrupt-index-v2.sqlite3" : "index-v2.sqlite3");
    const auto event = MakeHistorianEvent(
        "manyforge.program_runner", "program_failed", "ERROR", 1, "2026-09-08T00:00:00Z",
        CanonicalValue(CanonicalValue::Object{
            {"schema_version", CanonicalValue("manyforge.program_diagnostic.v1")},
            {"program_run_id", CanonicalValue("run.actual")},
            {"blackboard", CanonicalValue(CanonicalValue::Object{
                               {"operation_id", CanonicalValue("user.data")}})}}));
    {
      SqliteEventHistoryStore store(path);
      (void)store.Append(event);
    }
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(database,
                           MANYFORGE_RESTORE_PAYLOAD_SEARCH_INDEX
                           "DROP TABLE historian_capture_triggers; DROP TABLE historian_captures;"
                           "DROP TABLE historian_capture_retention; DROP INDEX historian_events_prune_idx;"
                           "ALTER TABLE historian_events DROP COLUMN capture_refs;"
                           "ALTER TABLE historian_events DROP COLUMN summary_json;"
                           "ALTER TABLE historian_events DROP COLUMN summary_digest;"
                           "UPDATE historian_metadata SET schema_version=2; PRAGMA "
                           "user_version=2;"
                           "INSERT INTO historian_event_keys(sequence,kind,value) "
                           "VALUES(1,'operation_id','user.data');",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    if (corrupt) {
      ASSERT_EQ(sqlite3_exec(database,
                             "UPDATE historian_event_keys SET value='tampered' "
                             "WHERE kind='operation_id'",
                             nullptr, nullptr, nullptr),
                SQLITE_OK);
    }
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    if (corrupt) {
      ExpectHistorianError([&] { SqliteEventHistoryStore rejected(path); },
                           "historian_storage_corrupt");
    } else {
      SqliteEventHistoryStore migrated(path);
      HistorianQuery query;
      query.correlations = {{"operation_id", "user.data"}};
      EXPECT_TRUE(migrated.Query(query).events.empty());
      query.correlations = {{"program_run_id", "run.actual"}};
      EXPECT_EQ(migrated.Query(query).events.size(), 1U);
      EXPECT_EQ(content_identity::canonical_json(migrated.Read(1)->event.payload),
                content_identity::canonical_json(event.payload));
    }
  }
}

TEST(EventHistorian, LargeStructuredPayloadsDoNotMakeListOrExportDecodeDiscardedRows) {
  TemporaryDirectory directory;
  SqliteEventHistoryStore store(directory.path() / "large-tree.sqlite3");
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
  store->SetFailing(true, "historian_storage_full");
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
  EXPECT_EQ(failed.error_code, "historian_storage_full");
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
  for (const auto *definition :
       {"manyforge.runtime.program_runner_loss",
        "manyforge.runtime.operation_journal_write_fault",
        "manyforge.runtime.kernel_internal_fault"}) {
    fact.definition_id = definition;
    for (const auto *type :
         {"alarm_raised", "alarm_acknowledged", "alarm_returned"}) {
      fact.event_type = type;
      const auto lifecycle = MakeAlarmLifecycleHistorianEvent(fact);
      const bool new_fault_raised = std::string_view(type) == "alarm_raised";
      EXPECT_EQ(lifecycle.severity, new_fault_raised ? "ERROR" : "INFO")
          << definition << " " << type;
    }
  }
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
  SqliteEventHistoryStore store(directory.path() / "capture.sqlite3", 4U, SmallCaptureLimits(),
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
  EXPECT_NE(retention.find("\"schema_version\":\"manyforge.historian_retention.v2\""), std::string::npos);
  EXPECT_NE(retention.find("\"protected_count\":\"6\""), std::string::npos);
  query.capture_id = 99U;
  ExpectHistorianError([&] { static_cast<void>(store.Query(query)); }, "historian_capture_not_found");
}

TEST(EventHistorian, FailureBurstJoinsTheOpenCaptureWithoutExtendingItsLimits) {
  TemporaryDirectory directory;
  FakeWriterClock clock;
  SqliteEventHistoryStore store(directory.path() / "burst.sqlite3", 100U, SmallCaptureLimits(),
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
  SqliteEventHistoryStore store(directory.path() / "resolution.sqlite3", 100U, SmallCaptureLimits(),
                                clock.Clock());
  const auto incident = [](const std::uint64_t index, const std::string &phase) {
    return MakeHistorianEvent(
        "manyforge.composer", "client_incident_" + phase, phase == "resolved" ? "INFO" : "ERROR",
        static_cast<std::int64_t>(100U + index), "2026-09-24T12:00:00Z",
        CanonicalValue(CanonicalValue::Object{
            {"incident_kind", CanonicalValue("viewer_scene_unverified")},
            {"original_severity", CanonicalValue("ERROR")},
            {"phase", CanonicalValue(phase)},
            {"schema_version", CanonicalValue("manyforge.client_incident_event.v1")},
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
  SqliteEventHistoryStore store(directory.path() / "full.sqlite3", 1000U, limits, clock.Clock());
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
  SqliteEventHistoryStore store(directory.path() / "late-loss.sqlite3", 100U, SmallCaptureLimits(),
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
  const auto path = directory.path() / "quiet.sqlite3";
  SqliteEventHistoryStore store(path, 100U, SmallCaptureLimits(), clock.Clock());
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
  SqliteEventHistoryStore store(directory.path() / "budget.sqlite3", 3U, SmallCaptureLimits(),
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
  const auto path = directory.path() / "restart.sqlite3";
  {
    FakeWriterClock clock;
    SqliteEventHistoryStore store(path, 100U, SmallCaptureLimits(), clock.Clock());
    ASSERT_EQ(store.Append(RoutineEvent(1)), IEventHistoryStore::AppendStatus::kStored);
    store.ObserveDeliveryLoss(2U);
    ASSERT_EQ(store.Append(ErrorEvent(2)), IEventHistoryStore::AppendStatus::kStored);
    store.ObserveDeliveryLoss(3U);
    ASSERT_EQ(store.Append(RoutineEvent(3)), IEventHistoryStore::AppendStatus::kStored);
  }
  {
    FakeWriterClock clock;
    SqliteEventHistoryStore restarted(path, 100U, SmallCaptureLimits(), clock.Clock());
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
  ExpectHistorianError([&] { SqliteEventHistoryStore rejected(path, 100U, SmallCaptureLimits()); },
                       "historian_storage_corrupt");
}

TEST(EventHistorian, Revision3MigrationAddsOnlyDerivedProtectionAndKeepsDigests) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "revision3.sqlite3";
  std::string original;
  {
    SqliteEventHistoryStore store(path, 100U, SmallCaptureLimits());
    ASSERT_EQ(store.Append(RoutineEvent(1)), IEventHistoryStore::AppendStatus::kStored);
    ASSERT_EQ(store.Append(RoutineEvent(2)), IEventHistoryStore::AppendStatus::kStored);
    original = content_identity::canonical_json(HistorianPersistenceValue(store.Read(2U)->event));
  }
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, kRestoreRevision3Schema, nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  SqliteEventHistoryStore migrated(path, 100U, SmallCaptureLimits());
  EXPECT_EQ(content_identity::canonical_json(HistorianPersistenceValue(migrated.Read(2U)->event)), original);
  EXPECT_TRUE(CaptureList(migrated).empty());
  ASSERT_EQ(migrated.Append(ErrorEvent(3)), IEventHistoryStore::AppendStatus::kStored);
  EXPECT_EQ(Field(CaptureList(migrated).front(), "first_sequence").as_number(), 1.0);
}

// Storage revision 5 replaces only the derived search text. A revision 4
// database indexed the whole payload JSON; it is rebuilt in place on open.
TEST(EventHistorian, Revision4SearchIndexMigratesToHumanTextAndKeepsOriginals) {
  TemporaryDirectory directory;
  for (const bool corrupt : {false, true}) {
    const auto path = directory.path() / (corrupt ? "corrupt-revision4.sqlite3" : "revision4.sqlite3");
    std::string original;
    {
      SqliteEventHistoryStore store(path, 100U);
      for (std::int64_t i = 1; i <= 3; ++i) {
        ASSERT_EQ(store.Append(MakeHistorianEvent("manyforge.runtime", "operation_progressed", "INFO", i,
            "2026-10-28T00:00:00Z", CanonicalValue(CanonicalValue::Object{
                {"message", CanonicalValue("visibleneedle " + std::to_string(i))},
                {"operation_id", CanonicalValue("operation.needle")},
                {"state", CanonicalValue(CanonicalValue::Object{{"error", CanonicalValue(CanonicalValue::Object{
                    {"code", CanonicalValue("nested_code")}})}})},
                {"blob", CanonicalValue("payloadneedle" + std::string(20000, static_cast<char>('a' + i)))}}))),
            IEventHistoryStore::AppendStatus::kStored);
      }
      original = content_identity::canonical_json(HistorianPersistenceValue(store.Read(3U)->event));
    }
    sqlite3 *database = nullptr;
    const auto scalar = [&](const char *sql) {
      sqlite3_stmt *statement = nullptr;
      EXPECT_EQ(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr), SQLITE_OK);
      EXPECT_EQ(sqlite3_step(statement), SQLITE_ROW);
      const auto value = sqlite3_column_int64(statement, 0);
      sqlite3_finalize(statement);
      return value;
    };
    const auto index_bytes = "SELECT sum(length(block)) FROM historian_search_data";
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(database, kRestorePayloadSearchIndex, nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(database, "UPDATE historian_metadata SET schema_version=4; PRAGMA user_version=4;",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_EQ(scalar("SELECT count(*) FROM historian_search WHERE historian_search MATCH '\"payloadneedle\"'"), 3);
    if (corrupt) {
      ASSERT_EQ(sqlite3_exec(database, "INSERT INTO historian_search(rowid,component,event_type,details) "
                             "VALUES(99,'fake','fake','gripper phantom')", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    const auto before = scalar(index_bytes);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    if (corrupt) {
      // As in revision 4, a damaged search index is refused, never rebuilt.
      ExpectHistorianError([&] { SqliteEventHistoryStore rejected(path, 100U); }, "historian_storage_corrupt");
      ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
      EXPECT_EQ(scalar("PRAGMA user_version"), 4);
      EXPECT_EQ(scalar("SELECT count(*) FROM historian_event_keys WHERE kind='code'"), 0);
      ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
      continue;
    }
    {
      SqliteEventHistoryStore migrated(path, 100U);
      EXPECT_EQ(content_identity::canonical_json(HistorianPersistenceValue(migrated.Read(3U)->event)), original);
      HistorianQuery query;
      query.search = "payloadneedle";
      EXPECT_TRUE(migrated.Query(query).events.empty());
      for (const auto *text : {"visibleneedle", "operation.needle", "nested_code"}) {
        query.search = text;
        EXPECT_EQ(migrated.Query(query).events.size(), 3U) << text;
      }
    }
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    EXPECT_EQ(scalar("PRAGMA user_version"), 6);
    const auto after = scalar(index_bytes);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    // Report numbers only; the size depends on the SQLite build.
    ::testing::Test::RecordProperty("search_index_bytes_before", std::to_string(before));
    ::testing::Test::RecordProperty("search_index_bytes_after", std::to_string(after));
    EXPECT_LT(after, before);
  }
}

// A revision 4 history whose old payload index filled the page ceiling still
// migrates: the old index is verified and dropped before code keys and the
// new index take pages, all in the one open transaction.
TEST(EventHistorian, NearCapacityRevision4MigrationDropsTheOldIndexFirst) {
  // Diagnostic only: flush each phase boundary so a timeout identifies the
  // in-flight phase as well as the durations of completed phases.
  auto phase_started = std::chrono::steady_clock::now();
  const char *phase = "setup";
  const auto next_phase = [&](const char *next) {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - phase_started).count();
    std::fprintf(stderr, "HISTORIAN_PHASE completed=%s elapsed_ms=%lld next=%s\n",
                 phase, static_cast<long long>(elapsed), next);
    std::fflush(stderr);
    phase_started = now;
    phase = next;
  };
  std::fprintf(stderr, "HISTORIAN_PHASE started=%s\n", phase);
  std::fflush(stderr);
  TemporaryDirectory directory;
  const auto path = directory.path() / "near-capacity-revision4.sqlite3";
  std::vector<std::string> originals;
  {
    SqliteEventHistoryStore store(path, 100U);
    next_phase("append_events");
    std::mt19937 random(7U);
    for (std::int64_t i = 1; i <= 60; ++i) {
      std::string blob(200000U, ' ');
      for (auto &character : blob) character = "abcdefghijklmnopqrstuvwxyz0123456789"[random() % 36U];
      CanonicalValue::Array codes;
      for (int code = 0; code < 2000; ++code)
        codes.emplace_back(CanonicalValue::Object{{"code", CanonicalValue("code." + std::to_string(code))}});
      const auto event = MakeHistorianEvent("manyforge.runtime", "operation_progressed", "INFO", i,
          "2026-10-28T00:00:00Z", CanonicalValue(CanonicalValue::Object{
              {"blob", CanonicalValue(std::move(blob))}, {"codes", CanonicalValue(std::move(codes))}}));
      ASSERT_EQ(store.Append(event), IEventHistoryStore::AppendStatus::kStored);
      originals.push_back(content_identity::canonical_json(HistorianPersistenceValue(event)));
    }
  }
  next_phase("restore_old_index");
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, kRestorePayloadSearchIndex, nullptr, nullptr, nullptr), SQLITE_OK);
  next_phase("prepare_pressure");
  // Grow the old index with unmerged segments (still consistent) until the
  // product page ceiling refuses the next one, leaving no free page.
  ASSERT_EQ(sqlite3_exec(database,
      "UPDATE historian_metadata SET schema_version=4; PRAGMA user_version=4; PRAGMA synchronous=OFF;"
      "INSERT INTO historian_search(historian_search,rank) VALUES('automerge',0);"
      "INSERT INTO historian_search(historian_search,rank) VALUES('crisismerge',2000);"
      "PRAGMA max_page_count=131072;", nullptr, nullptr, nullptr), SQLITE_OK);
  next_phase("fill_old_index_to_full");
  // Fill most of the index in batches. If a batch exceeds the remaining
  // capacity, roll it back and finish with the original single-row loop.
  // Each committed batch restores the same external-content index rows.
  const char *bulk_pressure =
      "BEGIN;"
      "INSERT INTO historian_search(historian_search,rowid,component,event_type,details) "
      "SELECT 'delete',sequence,component,event_type,details FROM historian_search_content;"
      "INSERT INTO historian_search(rowid,component,event_type,details) "
      "SELECT sequence,component,event_type,details FROM historian_search_content;"
      "COMMIT;";
  for (int batch = 0; batch < 10; ++batch) {
    if (sqlite3_exec(database, bulk_pressure, nullptr, nullptr, nullptr) != SQLITE_OK) {
      EXPECT_EQ(sqlite3_extended_errcode(database) & 0xff, SQLITE_FULL);
      if (!sqlite3_get_autocommit(database)) {
        ASSERT_EQ(sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
      }
      break;
    }
  }
  for (int cycle = 0;; ++cycle) {
    const auto sequence = std::to_string(cycle % 60 + 1);
    const auto sql = "BEGIN;"
        "INSERT INTO historian_search(historian_search,rowid,component,event_type,details) "
        "SELECT 'delete',sequence,component,event_type,details FROM historian_search_content WHERE sequence=" + sequence + ";"
        "INSERT INTO historian_search(rowid,component,event_type,details) "
        "SELECT sequence,component,event_type,details FROM historian_search_content WHERE sequence=" + sequence + ";"
        "COMMIT;";
    if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
      EXPECT_EQ(sqlite3_extended_errcode(database) & 0xff, SQLITE_FULL);
      if (!sqlite3_get_autocommit(database)) {
        ASSERT_EQ(sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
      }
      break;
    }
  }
  next_phase("fixture_integrity_and_close");
  ASSERT_EQ(sqlite3_exec(database, "INSERT INTO historian_search(historian_search,rank) VALUES('integrity-check',1)",
                         nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  EXPECT_GT(std::filesystem::file_size(path), 500U * 1024U * 1024U);
  {
    next_phase("migration_open");
    SqliteEventHistoryStore migrated(path, 100U);
    next_phase("verify_originals");
    for (std::size_t i = 0; i < originals.size(); ++i) {
      const auto stored = migrated.Read(i + 1U);
      ASSERT_TRUE(stored);
      EXPECT_EQ(content_identity::canonical_json(HistorianPersistenceValue(stored->event)), originals[i]);
    }
    // One 240 KB row per page keeps each query inside the 100 ms budget.
    next_phase("query_all_pages");
    HistorianQuery query;
    query.search = "code.1999";
    query.limit = 1U;
    std::size_t found = 0U;
    for (;;) {
      const auto page = migrated.Query(query);
      found += page.events.size();
      if (!page.next_before_sequence) break;
      query.before_sequence = page.next_before_sequence;
    }
    EXPECT_EQ(found, 60U);
  }
  next_phase("verify_version");
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  sqlite3_stmt *version = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &version, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(version), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(version, 0), 6);
  sqlite3_finalize(version);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
  next_phase("cleanup");
}

// Live finding: incident cause and closure codes were not
// search text, while the incident id was.
TEST(EventHistorian, IncidentCauseClosureAndVendorCodesAreSearchable) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "incident-search.sqlite3";
  const auto found = [](SqliteEventHistoryStore &store, const std::string &text) {
    HistorianQuery query;
    query.search = text;
    return store.Query(query).events.size();
  };
  for (const bool reopened : {false, true}) {
    SqliteEventHistoryStore store(path, 8U);
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

// Storage revision 6 adds the incident codes to the revision 5 search text.
// A revision 5 history is re-derived on open, so its existing incident rows
// become searchable; a damaged revision 5 index is still refused.
TEST(EventHistorian, Revision5SearchIndexMigratesToIncidentCodesAndKeepsOriginals) {
  TemporaryDirectory directory;
  constexpr std::int64_t kRows = 40;
  for (const bool corrupt : {false, true}) {
    const auto path = directory.path() / (corrupt ? "corrupt-revision5.sqlite3" : "revision5.sqlite3");
    std::string original;
    {
      SqliteEventHistoryStore store(path, 100U);
      for (std::int64_t i = 1; i <= kRows; ++i) {
        ASSERT_EQ(store.Append(ComponentIncidentEvent(i, i % 2 == 0 ? "provider_lost" : "emergency_stop", i % 4 == 0)),
                  IEventHistoryStore::AppendStatus::kStored);
      }
      original = content_identity::canonical_json(HistorianPersistenceValue(store.Read(4U)->event));
    }
    sqlite3 *database = nullptr;
    const auto scalar = [&](const char *sql) {
      sqlite3_stmt *statement = nullptr;
      EXPECT_EQ(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr), SQLITE_OK);
      EXPECT_EQ(sqlite3_step(statement), SQLITE_ROW);
      const auto value = sqlite3_column_int64(statement, 0);
      sqlite3_finalize(statement);
      return value;
    };
    const auto index_bytes = "SELECT (SELECT sum(length(block)) FROM historian_search_data)+"
                             "(SELECT sum(length(kind)+length(value)) FROM historian_event_keys)";
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(database, kRestoreRevision5SearchIndex.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_EQ(scalar("SELECT count(*) FROM historian_search WHERE historian_search MATCH '\"provider_lost\"'"), 0);
    EXPECT_EQ(scalar("SELECT count(*) FROM historian_search WHERE historian_search MATCH '\"incident.arm_robot.5\"'"), 1);
    if (corrupt) {
      ASSERT_EQ(sqlite3_exec(database, "INSERT INTO historian_search(rowid,component,event_type,details) "
                             "VALUES(99,'fake','fake','gripper phantom')", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    const auto before = scalar(index_bytes);
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    if (corrupt) {
      ExpectHistorianError([&] { SqliteEventHistoryStore rejected(path, 100U); }, "historian_storage_corrupt");
      ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
      EXPECT_EQ(scalar("PRAGMA user_version"), 5);
      EXPECT_EQ(scalar("SELECT count(*) FROM historian_event_keys WHERE kind='cause_class'"), 0);
      ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
      continue;
    }
    const auto found = [](SqliteEventHistoryStore &store, const std::string &text) {
      HistorianQuery query;
      query.search = text;
      query.limit = 32U;
      std::size_t count = 0U;
      for (;;) {
        const auto page = store.Query(query);
        count += page.events.size();
        if (!page.next_before_sequence) return count;
        query.before_sequence = page.next_before_sequence;
      }
    };
    {
      SqliteEventHistoryStore migrated(path, 100U);
      EXPECT_EQ(content_identity::canonical_json(HistorianPersistenceValue(migrated.Read(4U)->event)), original);
      EXPECT_EQ(found(migrated, "provider_lost"), static_cast<std::size_t>(kRows / 2));
      EXPECT_EQ(found(migrated, "emergency_stop"), static_cast<std::size_t>(kRows / 2));
      EXPECT_EQ(found(migrated, "cell_closed_by_operator"), static_cast<std::size_t>(kRows / 4));
      EXPECT_EQ(found(migrated, "mode=0"), static_cast<std::size_t>(kRows));
      EXPECT_EQ(found(migrated, "incident.arm_robot.5"), 1U);
      EXPECT_EQ(found(migrated, "summaryneedle"), 0U);
    }
    ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
    EXPECT_EQ(scalar("PRAGMA user_version"), 6);
    const auto after = scalar(index_bytes);
    const auto event_bytes = scalar("SELECT sum(length(payload_json)) FROM historian_events");
    ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
    // Report numbers; the bound (512 bytes per incident row) is well under
    // the size of a recorded incident row (about 3 KB).
    ::testing::Test::RecordProperty("search_index_bytes_revision5", std::to_string(before));
    ::testing::Test::RecordProperty("search_index_bytes_revision6", std::to_string(after));
    ::testing::Test::RecordProperty("event_payload_bytes", std::to_string(event_bytes));
    EXPECT_GT(after, before);
    EXPECT_LT(after - before, kRows * 512);
    {
      // New rows use the same derivation; a restart validates it.
      SqliteEventHistoryStore store(path, 100U);
      ASSERT_EQ(store.Append(ComponentIncidentEvent(kRows + 1, "provider_lost", true)),
                IEventHistoryStore::AppendStatus::kStored);
    }
    SqliteEventHistoryStore reopened(path, 100U);
    EXPECT_EQ(found(reopened, "provider_lost"), static_cast<std::size_t>(kRows / 2 + 1));
    EXPECT_EQ(found(reopened, "cell_closed_by_operator"), static_cast<std::size_t>(kRows / 4 + 1));
  }
}

TEST(EventHistorian, InconsistentCaptureLimitsAreRejected) {
  TemporaryDirectory directory;
  auto limits = SmallCaptureLimits();
  limits.protected_bytes = limits.capture_bytes - 1;
  ExpectHistorianError([&] { SqliteEventHistoryStore rejected(directory.path() / "limits.sqlite3", 100U, limits); },
                       "historian_configuration_invalid");
}

} // namespace
} // namespace manyforge::headless
