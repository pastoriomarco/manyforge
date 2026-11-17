#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

struct sqlite3;

namespace manyforge::persistence {

/// One write unit: the statements of one SQLite transaction with their bound
/// values. The writer executes it all-or-nothing.
class SqliteRecord final {
 public:
  SqliteRecord() = default;
  SqliteRecord(SqliteRecord&&) noexcept = default;
  SqliteRecord& operator=(SqliteRecord&&) noexcept = default;
  SqliteRecord(const SqliteRecord&) = delete;
  SqliteRecord& operator=(const SqliteRecord&) = delete;

  /// Appends the next statement; the following binds apply to it.
  SqliteRecord& Statement(std::string sql);
  SqliteRecord& Bind(int index, std::int64_t value);
  SqliteRecord& Bind(int index, std::string_view text);
  SqliteRecord& BindBlob(int index, std::string_view bytes);
  SqliteRecord& BindNull(int index);
  /// The current statement must change exactly this many rows.
  SqliteRecord& ExpectChanges(int changes);

  [[nodiscard]] bool empty() const noexcept { return statements_.empty(); }
  [[nodiscard]] std::size_t byte_size() const noexcept { return byte_size_; }

 private:
  friend class SqliteRecordWriter;

  struct Blob final {
    std::string bytes;
  };
  using Value =
      std::variant<std::nullptr_t, std::int64_t, std::string, Blob>;
  struct BoundStatement final {
    std::string sql;
    std::vector<std::pair<int, Value>> values;
    std::optional<int> expected_changes;
  };

  BoundStatement& Current();
  void Add(int index, Value value, std::size_t bytes);

  std::vector<BoundStatement> statements_;
  std::size_t byte_size_{};
};

enum class SqliteRecordNotWrittenReason {
  kQueueFull,
  kWriteFailed,
};

[[nodiscard]] std::string_view ToString(
    SqliteRecordNotWrittenReason reason) noexcept;

struct SqliteRecordWriterLimits final {
  std::size_t maximum_records;
  std::size_t maximum_bytes;
};

/// Queue bounds; not yet sized from a measured record rate.
inline constexpr SqliteRecordWriterLimits kSqliteRecordWriterDefaultLimits{
    4096U,
    64U * 1024U * 1024U,
};

/// Writes records to SQLite connections afterwards, on one thread, in the
/// order they were posted. Posting never waits; a record that does not fit the
/// bounded queue is dropped and counted. A record that fails is rolled back
/// and counted, and the writer carries on with the next one.
///
/// Per store, a loss episode starts at the first record not written and ends
/// when the queue has been written empty again. The writer thread prints one
/// line when an episode starts and, in the store's first episode, runs the
/// store's records-lost mark statement (again at a clean drain if that failed).
/// At stop it prints one line with the counts when anything was not written.
class SqliteRecordWriter final {
 public:
  struct StoreSlot final {
    std::size_t index;
  };

  explicit SqliteRecordWriter(
      SqliteRecordWriterLimits limits = kSqliteRecordWriterDefaultLimits);
  ~SqliteRecordWriter();

  SqliteRecordWriter(const SqliteRecordWriter&) = delete;
  SqliteRecordWriter& operator=(const SqliteRecordWriter&) = delete;
  SqliteRecordWriter(SqliteRecordWriter&&) = delete;
  SqliteRecordWriter& operator=(SqliteRecordWriter&&) = delete;

  /// Hands the connection to the writer thread. The caller does not use it
  /// again until DrainAndStop has returned. `records_lost_mark` is the one
  /// statement that sets the store's metadata mark that records of this start
  /// were not written.
  [[nodiscard]] StoreSlot RegisterConnection(
      sqlite3* connection,
      std::string store_name,
      std::string records_lost_mark);

  /// Queues the record without waiting. Returns false when it was dropped.
  bool Post(StoreSlot slot, SqliteRecord record);

  /// Writes every queued record, then joins the thread.
  void DrainAndStop();

  [[nodiscard]] std::uint64_t records_dropped() const noexcept {
    return records_dropped_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t records_not_written() const noexcept {
    return records_not_written_.load(std::memory_order_relaxed);
  }

 private:
  enum class LossMark { kNone, kWritten, kPending };
  // Every member is guarded by mutex_.
  struct Slot final {
    sqlite3* connection;
    std::string store_name;
    std::string records_lost_mark;
    bool drop_unreported;
    bool in_episode;
    LossMark mark;
    std::uint64_t dropped;
    std::uint64_t failed;
  };
  struct Queued final {
    std::size_t slot;
    SqliteRecord record;
  };

  void Run();
  [[nodiscard]] std::optional<std::string> Write(
      sqlite3* connection,
      const SqliteRecord& record);
  // The one place that decides what a record not written means. Writer
  // thread only, called under `lock`; it releases the lock for its line and
  // the mark.
  void OnRecordNotWritten(
      std::unique_lock<std::mutex>& lock,
      std::size_t slot,
      SqliteRecordNotWrittenReason reason,
      std::string_view detail);
  // Writer thread only, under `lock`: retries the pending marks after the
  // drain, then prints the shutdown line.
  void FinishStop(std::unique_lock<std::mutex>& lock);
  [[nodiscard]] bool WriteMark(
      sqlite3* connection,
      const std::string& records_lost_mark);

  const SqliteRecordWriterLimits limits_;
  std::mutex mutex_;
  std::condition_variable work_available_;
  std::deque<Queued> queue_;
  std::size_t queued_bytes_{};
  std::vector<Slot> slots_;
  bool stopping_{};
  bool drops_unreported_{};
  std::atomic<std::uint64_t> records_dropped_{};
  std::atomic<std::uint64_t> records_not_written_{};
  std::thread thread_;
};

}  // namespace manyforge::persistence
