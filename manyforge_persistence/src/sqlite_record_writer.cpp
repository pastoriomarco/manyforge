#include <manyforge_persistence/sqlite_record_writer.hpp>

#include <sqlite3.h>

#include <cstdio>
#include <type_traits>

namespace manyforge::persistence {

SqliteRecord::BoundStatement& SqliteRecord::Current() {
  return statements_.back();
}

void SqliteRecord::Add(const int index, Value value, const std::size_t bytes) {
  Current().values.emplace_back(index, std::move(value));
  byte_size_ += bytes + sizeof(std::pair<int, Value>);
}

SqliteRecord& SqliteRecord::Statement(std::string sql) {
  byte_size_ += sql.size() + sizeof(BoundStatement);
  statements_.push_back(BoundStatement{std::move(sql), {}, std::nullopt});
  return *this;
}

SqliteRecord& SqliteRecord::Bind(const int index, const std::int64_t value) {
  Add(index, value, 0U);
  return *this;
}

SqliteRecord& SqliteRecord::BindReal(const int index, const double value) {
  Add(index, value, 0U);
  return *this;
}

SqliteRecord& SqliteRecord::Bind(const int index, const std::string_view text) {
  Add(index, std::string(text), text.size());
  return *this;
}

SqliteRecord& SqliteRecord::BindBlob(
    const int index,
    const std::string_view bytes) {
  Add(index, Blob{std::string(bytes)}, bytes.size());
  return *this;
}

SqliteRecord& SqliteRecord::BindNull(const int index) {
  Add(index, nullptr, 0U);
  return *this;
}

SqliteRecord& SqliteRecord::ExpectChanges(const int changes) {
  Current().expected_changes = changes;
  return *this;
}

std::string_view ToString(const SqliteRecordNotWrittenReason reason) noexcept {
  switch (reason) {
    case SqliteRecordNotWrittenReason::kQueueFull:
      return "queue_full";
    case SqliteRecordNotWrittenReason::kWriteFailed:
      return "write_failed";
    case SqliteRecordNotWrittenReason::kNotDrained:
      return "not_drained";
  }
  return {};
}

SqliteRecordWriter::SqliteRecordWriter(const SqliteRecordWriterLimits limits)
    : limits_(limits), thread_([this] { Run(); }) {}

SqliteRecordWriter::~SqliteRecordWriter() { DrainAndStop(); }

SqliteRecordWriter::StoreSlot SqliteRecordWriter::RegisterConnection(
    sqlite3* const connection,
    std::string store_name,
    std::string records_lost_mark) {
  std::lock_guard lock(mutex_);
  slots_.push_back(Slot{
      connection,
      std::move(store_name),
      std::move(records_lost_mark),
      false,
      false,
      LossMark::kNone,
      0U,
      0U,
      0U});
  return StoreSlot{slots_.size() - 1U};
}

bool SqliteRecordWriter::Post(const StoreSlot slot, SqliteRecord record) {
  {
    std::lock_guard lock(mutex_);
    const auto bytes = record.byte_size();
    if (queue_.size() >= limits_.maximum_records ||
        bytes > limits_.maximum_bytes - queued_bytes_) {
      // Memory only: the writer thread reports the drop.
      auto& dropped = slots_[slot.index];
      ++dropped.dropped;
      dropped.drop_unreported = true;
      drops_unreported_ = true;
      records_dropped_.fetch_add(1U, std::memory_order_relaxed);
    } else {
      queued_bytes_ += bytes;
      queue_.push_back(Queued{slot.index, std::move(record)});
      work_available_.notify_one();
      return true;
    }
  }
  work_available_.notify_one();
  return false;
}

void SqliteRecordWriter::DrainAndStop(
    const std::optional<std::chrono::steady_clock::time_point> deadline) {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    deadline_ = deadline;
  }
  work_available_.notify_one();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void SqliteRecordWriter::Run() {
  std::unique_lock lock(mutex_);
  while (true) {
    work_available_.wait(lock, [this] {
      return stopping_ || drops_unreported_ || !queue_.empty();
    });
    if (drops_unreported_) {
      drops_unreported_ = false;
      for (std::size_t index = 0U; index < slots_.size(); ++index) {
        if (slots_[index].drop_unreported) {
          slots_[index].drop_unreported = false;
          OnRecordNotWritten(
              lock, index, SqliteRecordNotWrittenReason::kQueueFull, {});
        }
      }
      continue;
    }
    if (queue_.empty()) {
      FinishStop(lock, true);
      return;
    }
    if (stopping_ && deadline_.has_value() &&
        std::chrono::steady_clock::now() >= *deadline_) {
      for (const auto& queued : queue_) {
        ++slots_[queued.slot].not_drained;
      }
      records_not_written_.fetch_add(
          queue_.size(), std::memory_order_relaxed);
      queue_.clear();
      queued_bytes_ = 0U;
      FinishStop(lock, false);
      return;
    }
    auto queued = std::move(queue_.front());
    queue_.pop_front();
    queued_bytes_ -= queued.record.byte_size();
    sqlite3* const connection = slots_[queued.slot].connection;
    lock.unlock();
    const auto error = Write(connection, queued.record);
    lock.lock();
    if (error.has_value()) {
      ++slots_[queued.slot].failed;
      records_not_written_.fetch_add(1U, std::memory_order_relaxed);
      OnRecordNotWritten(
          lock,
          queued.slot,
          SqliteRecordNotWrittenReason::kWriteFailed,
          *error);
    } else if (queue_.empty() && !drops_unreported_) {
      // The queue was written empty: every loss episode ends.
      for (auto& slot : slots_) {
        slot.in_episode = false;
      }
    }
  }
}

std::optional<std::string> SqliteRecordWriter::Write(
    sqlite3* const connection,
    const SqliteRecord& record) {
  const auto rollback = [connection](std::string error) {
    sqlite3_exec(connection, "ROLLBACK", nullptr, nullptr, nullptr);
    return std::optional<std::string>(std::move(error));
  };
  if (sqlite3_exec(connection, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) !=
      SQLITE_OK) {
    return std::string("begin: ") + sqlite3_errmsg(connection);
  }
  for (const auto& bound : record.statements_) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            connection, bound.sql.c_str(), -1, &statement, nullptr) !=
        SQLITE_OK) {
      std::string error = std::string("prepare: ") + sqlite3_errmsg(connection);
      sqlite3_finalize(statement);
      return rollback(std::move(error));
    }
    int bind_result = SQLITE_OK;
    for (const auto& [index, value] : bound.values) {
      bind_result = std::visit(
          [statement, index](const auto& held) {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, std::nullptr_t>) {
              return sqlite3_bind_null(statement, index);
            } else if constexpr (std::is_same_v<Held, std::int64_t>) {
              return sqlite3_bind_int64(statement, index, held);
            } else if constexpr (std::is_same_v<Held, double>) {
              return sqlite3_bind_double(statement, index, held);
            } else if constexpr (std::is_same_v<Held, std::string>) {
              return sqlite3_bind_text(
                  statement,
                  index,
                  held.data(),
                  static_cast<int>(held.size()),
                  SQLITE_STATIC);
            } else {
              return sqlite3_bind_blob(
                  statement,
                  index,
                  held.bytes.data(),
                  static_cast<int>(held.bytes.size()),
                  SQLITE_STATIC);
            }
          },
          value);
      if (bind_result != SQLITE_OK) {
        break;
      }
    }
    int step_result = bind_result;
    if (bind_result == SQLITE_OK) {
      do {
        step_result = sqlite3_step(statement);
      } while (step_result == SQLITE_ROW);
    }
    if (step_result != SQLITE_DONE) {
      std::string error = std::string("statement: ") + sqlite3_errmsg(connection);
      sqlite3_finalize(statement);
      return rollback(std::move(error));
    }
    sqlite3_finalize(statement);
    if (bound.expected_changes.has_value() &&
        sqlite3_changes(connection) != *bound.expected_changes) {
      return rollback(
          "statement changed " + std::to_string(sqlite3_changes(connection)) +
          " rows, expected " + std::to_string(*bound.expected_changes));
    }
  }
  if (sqlite3_exec(connection, "COMMIT", nullptr, nullptr, nullptr) !=
      SQLITE_OK) {
    return rollback(std::string("commit: ") + sqlite3_errmsg(connection));
  }
  return std::nullopt;
}

void SqliteRecordWriter::OnRecordNotWritten(
    std::unique_lock<std::mutex>& lock,
    const std::size_t slot,
    const SqliteRecordNotWrittenReason reason,
    const std::string_view detail) {
  auto& store = slots_[slot];
  if (store.in_episode) {
    return;
  }
  store.in_episode = true;
  const bool write_mark = store.mark == LossMark::kNone;
  sqlite3* const connection = store.connection;
  const std::string store_name = store.store_name;
  const std::string records_lost_mark =
      write_mark ? store.records_lost_mark : std::string{};
  lock.unlock();
  const auto reason_text = ToString(reason);
  std::fprintf(
      stderr,
      "store records are not being written: store=%s reason=%.*s "
      "detail=%.*s; counting them until the queue is written empty again\n",
      store_name.c_str(),
      static_cast<int>(reason_text.size()),
      reason_text.data(),
      static_cast<int>(detail.size()),
      detail.data());
  const bool written = write_mark && WriteMark(connection, records_lost_mark);
  lock.lock();
  if (write_mark) {
    slots_[slot].mark = written ? LossMark::kWritten : LossMark::kPending;
  }
}

void SqliteRecordWriter::FinishStop(
    std::unique_lock<std::mutex>& lock,
    const bool drained) {
  if (drained) {
    for (std::size_t index = 0U; index < slots_.size(); ++index) {
      if (slots_[index].mark != LossMark::kPending) {
        continue;
      }
      sqlite3* const connection = slots_[index].connection;
      const std::string records_lost_mark = slots_[index].records_lost_mark;
      lock.unlock();
      const bool written = WriteMark(connection, records_lost_mark);
      lock.lock();
      if (written) {
        slots_[index].mark = LossMark::kWritten;
      }
    }
  }
  std::string counts;
  for (const auto& store : slots_) {
    if (store.dropped + store.failed + store.not_drained != 0U) {
      counts += " store=" + store.store_name +
                " queue_full=" + std::to_string(store.dropped) +
                " write_failed=" + std::to_string(store.failed) +
                " not_drained=" + std::to_string(store.not_drained) + ";";
    }
  }
  lock.unlock();
  if (!counts.empty()) {
    std::fprintf(
        stderr,
        "store records not written in this start:%s\n",
        counts.c_str());
  }
  lock.lock();
}

bool SqliteRecordWriter::WriteMark(
    sqlite3* const connection,
    const std::string& records_lost_mark) {
  SqliteRecord mark;
  mark.Statement(records_lost_mark);
  return !Write(connection, mark).has_value();
}

}  // namespace manyforge::persistence
