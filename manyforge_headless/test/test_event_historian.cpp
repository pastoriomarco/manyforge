#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

#include "manyforge_headless/event_historian.hpp"

namespace manyforge::headless {
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
    EXPECT_EQ(exact->event.event_id, Event(4U, "manyforge.scene",
                                          "operator_alarm_returned")
                                         .event_id);
    EXPECT_FALSE(store.Read(2U).has_value());
  }

  SqliteEventHistoryStore reopened(path, 3U);
  const auto page = reopened.Query(HistorianQuery{});
  ASSERT_EQ(page.events.size(), 3U);
  EXPECT_EQ(page.events.front().event.event_id, Event(5U).event_id);
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
  EXPECT_NE(encoded.find("manyforge.historian_detail.v1"), std::string::npos);
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
  SqliteEventHistoryStore tampered(target, 8U);
  ExpectHistorianError(
      [&]() { static_cast<void>(tampered.Query(HistorianQuery{})); },
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
