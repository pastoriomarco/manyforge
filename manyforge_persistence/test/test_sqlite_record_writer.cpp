#include <manyforge_persistence/sqlite_record_writer.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace manyforge::persistence {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-record-writer-test-XXXXXX")
                       .string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (mkdtemp(buffer.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = buffer.data();
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// Scripted failure point for the mark: it fails while `allowed` is false.
struct MarkSwitch final {
  std::atomic<bool> allowed{true};
};

void MarkAllowedFunction(sqlite3_context* context, int, sqlite3_value**) {
  if (!static_cast<MarkSwitch*>(sqlite3_user_data(context))->allowed) {
    sqlite3_result_error(context, "mark refused by the test", -1);
    return;
  }
  sqlite3_result_int(context, 1);
}

class Database final {
 public:
  explicit Database(const std::filesystem::path& path) {
    if (sqlite3_open(path.c_str(), &handle_) != SQLITE_OK) {
      throw std::runtime_error("open failed");
    }
    Exec(
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
        "CREATE TABLE rows(id INTEGER PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE metadata(singleton INTEGER PRIMARY KEY,"
        " records_lost INTEGER NOT NULL DEFAULT 0,"
        " marks INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO metadata(singleton) VALUES (1);");
    if (sqlite3_create_function(
            handle_,
            "mark_allowed",
            0,
            SQLITE_UTF8,
            &mark_switch,
            MarkAllowedFunction,
            nullptr,
            nullptr) != SQLITE_OK) {
      throw std::runtime_error("mark_allowed failed");
    }
  }
  ~Database() { sqlite3_close(handle_); }
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  void Exec(const std::string& sql) {
    if (sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(handle_));
    }
  }

  [[nodiscard]] std::vector<std::string> Values() {
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        handle_, "SELECT value FROM rows ORDER BY id", -1, &statement, nullptr);
    std::vector<std::string> values;
    while (sqlite3_step(statement) == SQLITE_ROW) {
      values.emplace_back(
          reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
    }
    sqlite3_finalize(statement);
    return values;
  }

  // The mark and how often the writer ran its statement.
  [[nodiscard]] std::pair<std::int64_t, std::int64_t> Mark() {
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        handle_,
        "SELECT records_lost, marks FROM metadata",
        -1,
        &statement,
        nullptr);
    sqlite3_step(statement);
    const std::pair<std::int64_t, std::int64_t> mark{
        sqlite3_column_int64(statement, 0), sqlite3_column_int64(statement, 1)};
    sqlite3_finalize(statement);
    return mark;
  }

  [[nodiscard]] sqlite3* handle() const { return handle_; }

  MarkSwitch mark_switch;

 private:
  sqlite3* handle_{};
};

// The test stores' records-lost mark; `marks` counts how often it ran.
constexpr const char* kMark =
    "UPDATE metadata SET records_lost = 1, marks = marks + 1 "
    "WHERE singleton = 1 AND mark_allowed()";

std::size_t Count(const std::string& text, const std::string& needle) {
  std::size_t count = 0U;
  for (auto position = text.find(needle); position != std::string::npos;
       position = text.find(needle, position + needle.size())) {
    ++count;
  }
  return count;
}

constexpr const char* kEpisodeLine = "store records are not being written:";

SqliteRecord Insert(const std::int64_t id, const std::string& value) {
  SqliteRecord record;
  record.Statement("INSERT INTO rows(id,value) VALUES (?,?)")
      .Bind(1, id)
      .Bind(2, value);
  return record;
}

// Blocks the writer thread inside a statement until the test releases it.
struct Gate final {
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released{release.get_future().share()};
};

void GateFunction(sqlite3_context* context, int, sqlite3_value**) {
  auto* gate = static_cast<Gate*>(sqlite3_user_data(context));
  gate->entered.set_value();
  gate->released.wait();
  sqlite3_result_int(context, 1);
}

void InstallGate(sqlite3* handle, Gate& gate, const char* name = "gate") {
  ASSERT_EQ(
      sqlite3_create_function(
          handle, name, 0, SQLITE_UTF8, &gate, GateFunction, nullptr, nullptr),
      SQLITE_OK);
}

SqliteRecord GateRecord(const std::string& name = "gate") {
  SqliteRecord record;
  record.Statement("SELECT " + name + "()");
  return record;
}

TEST(SqliteRecordWriter, WritesInPostOrderAcrossConnections) {
  TemporaryDirectory directory;
  Database first(directory.path() / "first.sqlite3");
  Database second(directory.path() / "second.sqlite3");
  std::vector<std::string> order;
  const auto log = [](sqlite3_context* context, int, sqlite3_value** values) {
    static_cast<std::vector<std::string>*>(sqlite3_user_data(context))
        ->emplace_back(
            reinterpret_cast<const char*>(sqlite3_value_text(values[0])));
    sqlite3_result_null(context);
  };
  for (auto* handle : {first.handle(), second.handle()}) {
    ASSERT_EQ(
        sqlite3_create_function(
            handle, "log_order", 1, SQLITE_UTF8, &order, log, nullptr, nullptr),
        SQLITE_OK);
  }
  SqliteRecordWriter writer;
  const auto first_slot = writer.RegisterConnection(first.handle(), "first", kMark);
  const auto second_slot = writer.RegisterConnection(second.handle(), "second", kMark);
  for (int index = 0; index < 20; ++index) {
    SqliteRecord record;
    record.Statement("SELECT log_order(?)").Bind(1, std::to_string(index));
    ASSERT_TRUE(
        writer.Post(index % 2 == 0 ? first_slot : second_slot, std::move(record)));
  }
  writer.DrainAndStop();
  ASSERT_EQ(order.size(), 20U);
  for (int index = 0; index < 20; ++index) {
    EXPECT_EQ(order[static_cast<std::size_t>(index)], std::to_string(index));
  }
  EXPECT_EQ(writer.records_dropped(), 0U);
  EXPECT_EQ(writer.records_not_written(), 0U);
}

TEST(SqliteRecordWriter, FullQueueDropsAndCountsWithoutWaiting) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  Gate gate;
  InstallGate(database.handle(), gate);
  SqliteRecordWriter writer({2U, 1024U * 1024U});
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);
  ASSERT_TRUE(writer.Post(slot, GateRecord()));
  gate.entered.get_future().wait();
  // The writer thread is held inside the gate; posting must still return.
  EXPECT_TRUE(writer.Post(slot, Insert(1, "kept-1")));
  EXPECT_TRUE(writer.Post(slot, Insert(2, "kept-2")));
  EXPECT_FALSE(writer.Post(slot, Insert(3, "dropped")));
  EXPECT_EQ(writer.records_dropped(), 1U);
  gate.release.set_value();
  writer.DrainAndStop();
  EXPECT_EQ(database.Values(), (std::vector<std::string>{"kept-1", "kept-2"}));
  EXPECT_EQ(writer.records_not_written(), 0U);
}

TEST(SqliteRecordWriter, ByteBoundDropsOversizedRecord) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  SqliteRecordWriter writer({16U, 256U});
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);
  EXPECT_FALSE(writer.Post(slot, Insert(1, std::string(512U, 'x'))));
  EXPECT_EQ(writer.records_dropped(), 1U);
  writer.DrainAndStop();
  EXPECT_TRUE(database.Values().empty());
}

TEST(SqliteRecordWriter, FailedRecordIsCountedAndNextRecordIsWritten) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  SqliteRecordWriter writer;
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);
  ASSERT_TRUE(writer.Post(slot, Insert(1, "first")));
  ASSERT_TRUE(writer.Post(slot, Insert(1, "duplicate key")));
  ASSERT_TRUE(writer.Post(slot, Insert(2, "second")));
  writer.DrainAndStop();
  EXPECT_EQ(database.Values(), (std::vector<std::string>{"first", "second"}));
  EXPECT_EQ(writer.records_not_written(), 1U);
}

TEST(SqliteRecordWriter, RecordIsAllOrNothing) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  database.Exec("INSERT INTO rows(id,value) VALUES (7,'existing')");
  SqliteRecordWriter writer;
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);
  SqliteRecord record;
  record.Statement("INSERT INTO rows(id,value) VALUES (?,?)")
      .Bind(1, std::int64_t{1})
      .Bind(2, "part-one")
      .Statement("INSERT INTO rows(id,value) VALUES (?,?)")
      .Bind(1, std::int64_t{7})
      .Bind(2, "conflict");
  ASSERT_TRUE(writer.Post(slot, std::move(record)));
  writer.DrainAndStop();
  EXPECT_EQ(database.Values(), (std::vector<std::string>{"existing"}));
  EXPECT_EQ(writer.records_not_written(), 1U);
}

TEST(SqliteRecordWriter, UnexpectedChangeCountRollsBack) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  SqliteRecordWriter writer;
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);
  SqliteRecord record;
  record.Statement("INSERT INTO rows(id,value) VALUES (1,'inserted')")
      .Statement("UPDATE rows SET value='updated' WHERE id=?")
      .Bind(1, std::int64_t{99})
      .ExpectChanges(1);
  ASSERT_TRUE(writer.Post(slot, std::move(record)));
  writer.DrainAndStop();
  EXPECT_TRUE(database.Values().empty());
  EXPECT_EQ(writer.records_not_written(), 1U);
}

TEST(SqliteRecordWriter, DrainAndStopWritesEverythingQueued) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  Gate gate;
  InstallGate(database.handle(), gate);
  SqliteRecordWriter writer;
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);
  ASSERT_TRUE(writer.Post(slot, GateRecord()));
  gate.entered.get_future().wait();
  for (int index = 1; index <= 100; ++index) {
    ASSERT_TRUE(writer.Post(slot, Insert(index, std::to_string(index))));
  }
  gate.release.set_value();
  writer.DrainAndStop();
  EXPECT_EQ(database.Values().size(), 100U);
}

TEST(SqliteRecordWriter, DeadlineCountsRecordsNotDrained) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  Gate gate;
  InstallGate(database.handle(), gate);
  SqliteRecordWriter writer;
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);
  ASSERT_TRUE(writer.Post(slot, GateRecord()));
  gate.entered.get_future().wait();
  ASSERT_TRUE(writer.Post(slot, Insert(1, "late")));
  // The release may reach the writer before or after the stop; either way
  // the record is written or counted, never lost silently.
  std::thread release([&gate] { gate.release.set_value(); });
  writer.DrainAndStop(std::chrono::steady_clock::time_point::min());
  release.join();
  EXPECT_EQ(database.Values().size() + writer.records_not_written(), 1U);
}

TEST(SqliteRecordWriter, FullQueueStartsOneEpisodeUntilTheQueueIsWrittenEmpty) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  Gate first_gate;
  Gate second_gate;
  InstallGate(database.handle(), first_gate, "first_gate");
  InstallGate(database.handle(), second_gate, "second_gate");
  testing::internal::CaptureStderr();
  SqliteRecordWriter writer({1U, 1024U * 1024U});
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);

  ASSERT_TRUE(writer.Post(slot, GateRecord("first_gate")));
  first_gate.entered.get_future().wait();
  ASSERT_TRUE(writer.Post(slot, Insert(1, "kept-1")));
  for (int index = 0; index < 3; ++index) {
    EXPECT_FALSE(writer.Post(slot, Insert(100 + index, "dropped")));
  }
  first_gate.release.set_value();
  // The only timing assumption: the writer writes `kept-1`, finds the queue
  // empty and ends the episode within this settle time.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // A new loss after the queue was written empty starts a second episode.
  ASSERT_TRUE(writer.Post(slot, GateRecord("second_gate")));
  second_gate.entered.get_future().wait();
  ASSERT_TRUE(writer.Post(slot, Insert(2, "kept-2")));
  EXPECT_FALSE(writer.Post(slot, Insert(200, "dropped")));
  EXPECT_FALSE(writer.Post(slot, Insert(201, "dropped")));
  second_gate.release.set_value();
  writer.DrainAndStop();
  const auto output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(Count(output, kEpisodeLine), 2U) << output;
  EXPECT_EQ(Count(output, "store=store reason=queue_full"), 2U) << output;
  EXPECT_EQ(
      Count(
          output,
          "store records not written in this start: store=store "
          "queue_full=5 write_failed=0 not_drained=0;\n"),
      1U)
      << output;
  EXPECT_EQ(writer.records_dropped(), 5U);
  EXPECT_EQ(writer.records_not_written(), 0U);
  // Set once, in the first episode, and kept after the clean close.
  EXPECT_EQ(database.Mark(), (std::pair<std::int64_t, std::int64_t>{1, 1}));
  EXPECT_EQ(database.Values(), (std::vector<std::string>{"kept-1", "kept-2"}));
}

TEST(SqliteRecordWriter, FailedWritesStartOneEpisodeOnlyForTheirStore) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  Database other(directory.path() / "other.sqlite3");
  Gate gate;
  InstallGate(database.handle(), gate);
  testing::internal::CaptureStderr();
  SqliteRecordWriter writer;
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);
  const auto other_slot =
      writer.RegisterConnection(other.handle(), "other", kMark);
  ASSERT_TRUE(writer.Post(slot, GateRecord()));
  gate.entered.get_future().wait();
  ASSERT_TRUE(writer.Post(slot, Insert(1, "first")));
  ASSERT_TRUE(writer.Post(slot, Insert(1, "duplicate key")));
  ASSERT_TRUE(writer.Post(other_slot, Insert(1, "other")));
  ASSERT_TRUE(writer.Post(slot, Insert(1, "duplicate again")));
  gate.release.set_value();
  writer.DrainAndStop();
  const auto output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(Count(output, kEpisodeLine), 1U) << output;
  EXPECT_EQ(Count(output, "store=store reason=write_failed detail="), 1U)
      << output;
  EXPECT_EQ(
      Count(
          output,
          "store records not written in this start: store=store "
          "queue_full=0 write_failed=2 not_drained=0;\n"),
      1U)
      << output;
  EXPECT_EQ(writer.records_not_written(), 2U);
  EXPECT_EQ(database.Mark(), (std::pair<std::int64_t, std::int64_t>{1, 1}));
  EXPECT_EQ(other.Mark(), (std::pair<std::int64_t, std::int64_t>{0, 0}));
  EXPECT_EQ(other.Values(), (std::vector<std::string>{"other"}));
}

TEST(SqliteRecordWriter, MarkThatFailedIsWrittenAtTheCleanDrain) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  Gate gate;
  InstallGate(database.handle(), gate);
  database.mark_switch.allowed = false;
  testing::internal::CaptureStderr();
  SqliteRecordWriter writer;
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);
  ASSERT_TRUE(writer.Post(slot, Insert(1, "first")));
  ASSERT_TRUE(writer.Post(slot, Insert(1, "duplicate key")));
  ASSERT_TRUE(writer.Post(slot, GateRecord()));
  gate.entered.get_future().wait();
  // The first attempt has run and failed; the clean drain writes it once.
  database.mark_switch.allowed = true;
  gate.release.set_value();
  writer.DrainAndStop();
  static_cast<void>(testing::internal::GetCapturedStderr());
  EXPECT_EQ(database.Mark(), (std::pair<std::int64_t, std::int64_t>{1, 1}));
}

TEST(SqliteRecordWriter, NoLossLeavesTheMarkClearAndPrintsNothing) {
  TemporaryDirectory directory;
  Database database(directory.path() / "store.sqlite3");
  testing::internal::CaptureStderr();
  SqliteRecordWriter writer;
  const auto slot = writer.RegisterConnection(database.handle(), "store", kMark);
  ASSERT_TRUE(writer.Post(slot, Insert(1, "first")));
  writer.DrainAndStop();
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
  EXPECT_EQ(database.Mark(), (std::pair<std::int64_t, std::int64_t>{0, 0}));
}

}  // namespace
}  // namespace manyforge::persistence
