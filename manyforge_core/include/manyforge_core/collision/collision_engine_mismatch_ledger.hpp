#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

#include <manyforge_core/types/collision.hpp>

namespace manyforge::core {

/// Bounded ring buffer of CollisionEngineMismatchRecord values.
///
/// The ledger lets compare-mode emitters (for example a RoboPlan diagnostic
/// comparator) deposit canonical mismatch records that
/// can later be inspected by failure-archive integrations, qualification
/// harnesses, or operator-facing diagnostics. The ledger is thread-safe so
/// that compare-mode emission paths can push without contention against
/// readers.
///
/// The ledger does NOT decide disposition. The caller (deployment / kernel /
/// qualification harness) sets `record.disposition` per the configured
/// CollisionEngineCompareMode before pushing.
class CollisionEngineMismatchLedger {
 public:
  explicit CollisionEngineMismatchLedger(std::size_t capacity = 64)
      : capacity_(capacity) {}

  /// Capacity in record-count. Older records are dropped when the ledger
  /// is at capacity and a new record is pushed.
  std::size_t capacity() const { return capacity_; }

  /// Push one mismatch record. Thread-safe.
  void push(CollisionEngineMismatchRecord record) {
    std::scoped_lock lock(mutex_);
    total_pushed_++;
    if (capacity_ == 0) {
      return;
    }
    if (entries_.size() >= capacity_) {
      entries_.pop_front();
    }
    entries_.push_back(std::move(record));
  }

  /// Retrieve the most-recent `limit` records (newest first). limit==0 yields
  /// every retained record. Returns by value so the caller can iterate
  /// without holding the ledger lock.
  std::vector<CollisionEngineMismatchRecord> recent(std::size_t limit = 0) const {
    std::scoped_lock lock(mutex_);
    std::vector<CollisionEngineMismatchRecord> out;
    if (entries_.empty()) {
      return out;
    }
    const std::size_t take =
        limit == 0 ? entries_.size() : std::min(limit, entries_.size());
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
      out.push_back(entries_[entries_.size() - 1 - i]);
    }
    return out;
  }

  /// Total number of records that have been pushed, including dropped ones.
  std::uint64_t total_pushed() const {
    std::scoped_lock lock(mutex_);
    return total_pushed_;
  }

  /// Drop all retained records. Total-pushed counter is preserved.
  void clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
  }

 private:
  mutable std::mutex mutex_;
  std::size_t capacity_;
  std::deque<CollisionEngineMismatchRecord> entries_;
  std::uint64_t total_pushed_ = 0;
};

}  // namespace manyforge::core
