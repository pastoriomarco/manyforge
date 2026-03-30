#pragma once

#include <manyforge_core/time/ischeduler.hpp>

#include <map>
#include <utility>
#include <vector>

namespace manyforge::core {

/// A manual scheduler for testing that fires callbacks when time is advanced.
/// Not thread-safe. Intended for deterministic unit tests.
class ManualScheduler final : public IScheduler {
 public:
  /// Constructs a ManualScheduler.
  ManualScheduler() : next_handle_(1) {}

  /// Schedule a callback to fire at the given deadline.
  ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback) override {
    ScheduleHandle handle = next_handle_++;
    pending_.emplace(deadline, std::make_pair(handle, std::move(callback)));
    return handle;
  }

  /// Cancel a scheduled callback. No-op if already fired or invalid.
  void cancel(ScheduleHandle handle) override {
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

    for (auto it = pending_.begin(); it != pending_.end(); ) {
      if (it->first <= t) {
        to_fire.push_back(std::move(it->second.second));
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }

    for (auto& callback : to_fire) {
      callback();
    }
  }

 private:
  /// Multimap from deadline to (handle, callback).
  /// Allows multiple callbacks at the same deadline; sorted by deadline.
  std::multimap<TimePoint, std::pair<ScheduleHandle, std::function<void()>>> pending_;
  uint64_t next_handle_;
};

}  // namespace manyforge::core
