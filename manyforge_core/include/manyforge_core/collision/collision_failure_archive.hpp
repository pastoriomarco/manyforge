#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include <manyforge_core/types/collision.hpp>

namespace manyforge::core {

/// Bounded ring buffer of CollisionFailureArchiveEntry values.
///
/// The archive is the in-process v0 of a failure-triage surface. The
/// `CollisionValidationCoordinator` (or any caller wrapping it) pushes
/// one entry per non-`kDispatch` decision. Archive consumers can:
///   - inspect the most-recent N entries for operator-facing diagnostics
///   - drain via the optional sink callback to a long-retention store
///     (JSONL file, telemetry bus, qualification harness) — the
///     persistence format is deliberately not decided yet).
///
/// Thread-safety: `push`/`recent`/`clear` are all safe from any thread.
/// The optional sink callback is copied under the archive lock and then
/// invoked after the lock is released. Sinks still SHOULD be fast
/// because they run synchronously on the caller's push path; slow sinks
/// should enqueue to their own worker or drain via `recent()` from a
/// dedicated drainer thread instead.
///
/// The archive does NOT decide whether a record should be pushed; the
/// caller (coordinator) decides based on the kernel-facing decision.
/// Passing trajectories MUST NOT be pushed — see the DTO doc for why.
class CollisionFailureArchive {
 public:
  using SinkFn = std::function<void(const CollisionFailureArchiveEntry&)>;

  explicit CollisionFailureArchive(std::size_t capacity = 64)
      : capacity_(capacity) {}

  /// Capacity in entry-count. Older entries are dropped when the archive
  /// is at capacity and a new entry is pushed.
  std::size_t capacity() const { return capacity_; }

  /// Install (or replace) the sink. Pass an empty std::function to
  /// detach. The sink fires on every push (even when the ring buffer is
  /// at capacity); consumers that only want a tail SHOULD also call
  /// `recent()` rather than relying on sink ordering across drops.
  void set_sink(SinkFn sink) {
    std::scoped_lock lock(mutex_);
    sink_ = std::move(sink);
  }

  /// Push one failure entry. Thread-safe. The sink (if any) fires
  /// synchronously after the ring-buffer mutation is complete and after
  /// the archive lock is released.
  void push(CollisionFailureArchiveEntry entry) {
    SinkFn sink;
    CollisionFailureArchiveEntry sink_entry;
    {
      std::scoped_lock lock(mutex_);
      total_pushed_++;
      if (capacity_ == 0) {
        total_dropped_++;
        sink = sink_;
        sink_entry = std::move(entry);
      } else {
        if (entries_.size() >= capacity_) {
          entries_.pop_front();
          total_dropped_++;
        }
        entries_.push_back(std::move(entry));
        sink = sink_;
        sink_entry = entries_.back();
      }
    }
    if (sink) {
      sink(sink_entry);
    }
  }

  /// Most-recent `limit` entries (newest first). limit==0 yields every
  /// retained entry. Returns by value so the caller can iterate without
  /// holding the archive lock.
  std::vector<CollisionFailureArchiveEntry> recent(
      std::size_t limit = 0) const {
    std::scoped_lock lock(mutex_);
    std::vector<CollisionFailureArchiveEntry> out;
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

  /// Total entries ever pushed (including dropped).
  std::uint64_t total_pushed() const {
    std::scoped_lock lock(mutex_);
    return total_pushed_;
  }

  /// Total entries dropped due to capacity overflow.
  std::uint64_t total_dropped() const {
    std::scoped_lock lock(mutex_);
    return total_dropped_;
  }

  /// Drop all retained entries. Total-pushed / total-dropped counters
  /// are preserved.
  void clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
  }

 private:
  mutable std::mutex mutex_;
  std::size_t capacity_;
  std::deque<CollisionFailureArchiveEntry> entries_;
  std::uint64_t total_pushed_ = 0;
  std::uint64_t total_dropped_ = 0;
  SinkFn sink_;
};

}  // namespace manyforge::core
