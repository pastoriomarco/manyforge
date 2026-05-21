#pragma once

#include <manyforge_core/time/ischeduler.hpp>

#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace manyforge::core {

/// A manual scheduler for testing that fires callbacks when time is advanced.
/// Thread-safe for concurrent schedule/cancel/advance calls; callbacks run
/// outside the scheduler lock.
class ManualScheduler final : public IScheduler {
 public:
  /// Constructs a ManualScheduler.
  ManualScheduler() : next_handle_(1) {}

  /// Schedule a callback to fire at the given deadline.
  ///
  /// `priority` is accepted for IScheduler conformance and stored for
  /// inspection in tests but does not affect deadline ordering: the
  /// manual scheduler is single-threaded by design and reports
  /// callbacks via `advance_to`. Production schedulers split priority
  /// across worker pools; see `SchedulerPriority`.
  ScheduleHandle schedule(
      TimePoint deadline, std::function<void()> callback,
      SchedulerPriority priority = SchedulerPriority::Safety) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ScheduleHandle handle = next_handle_++;
    last_priority_ = priority;
    pending_.emplace(deadline, std::make_pair(handle, std::move(callback)));
    return handle;
  }

  /// Test-only: returns the priority passed to the most recent schedule()
  /// call. Useful for asserting that production callers tag their
  /// callbacks correctly.
  SchedulerPriority last_priority() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_priority_;
  }

  /// Cancel a scheduled callback. No-op if already fired or invalid.
  void cancel(ScheduleHandle handle) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->second.first == handle) {
        pending_.erase(it);
        return;
      }
    }
  }

  /// Advance time to the given point, firing all callbacks with deadline <= t.
  /// Callbacks are fired in deadline order (earliest first).
  void advance_to(TimePoint t) {
    std::vector<std::function<void()>> to_fire;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = pending_.begin(); it != pending_.end(); ) {
        if (it->first <= t) {
          to_fire.push_back(std::move(it->second.second));
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }

    for (auto& callback : to_fire) {
      callback();
    }
  }

 private:
  mutable std::mutex mutex_;
  /// Multimap from deadline to (handle, callback).
  /// Allows multiple callbacks at the same deadline; sorted by deadline.
  std::multimap<TimePoint, std::pair<ScheduleHandle, std::function<void()>>> pending_;
  uint64_t next_handle_;
  SchedulerPriority last_priority_ = SchedulerPriority::Safety;
};

}  // namespace manyforge::core
