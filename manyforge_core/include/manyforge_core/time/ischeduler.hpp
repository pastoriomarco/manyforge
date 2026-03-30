#pragma once

#include <cstdint>
#include <functional>

#include <manyforge_core/time/iclock.hpp>

namespace manyforge::core {

/// Handle for a scheduled callback. Used to cancel.
using ScheduleHandle = uint64_t;

/// Abstract scheduler interface for deferred callbacks.
/// ADR-0004: no std::this_thread::sleep_for in core.
/// Production: backed by rclcpp timers or a timer thread.
/// Tests: use ManualScheduler for deterministic advancement.
class IScheduler {
 public:
  virtual ~IScheduler() = default;

  /// Schedule a callback to fire at or after the given deadline.
  /// Returns a handle that can be used to cancel.
  virtual ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback) = 0;

  /// Cancel a previously scheduled callback. No-op if already fired or invalid.
  virtual void cancel(ScheduleHandle handle) = 0;
};

}  // namespace manyforge::core
