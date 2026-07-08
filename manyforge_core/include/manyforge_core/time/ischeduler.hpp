#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include <manyforge_core/time/iclock.hpp>

namespace manyforge::core {

/// Handle for a scheduled callback. Used to cancel.
using ScheduleHandle = uint64_t;

/// Worker-pool affinity hint for scheduled callbacks.
///
/// Production schedulers (SimpleScheduler) MUST keep these roles on independent
/// worker threads or on an explicitly documented stronger execution substrate.
/// The roles are ordered by runtime criticality, but the primary invariant is
/// isolation: blocking lifecycle/background work must not starve controller
/// command progress, runtime-safety ticks, or stop-control watchdogs.
///
/// Test schedulers (ManualScheduler) and adapters that single-thread callbacks
/// SHOULD treat the value as advisory while preserving it for assertions.
enum class SchedulerPriority {
  ControllerCommand = 0,
  RuntimeSafety = 1,
  StopControl = 2,
  Lifecycle = 3,
  Monitoring = 4,
  Background = 5,

  // Deprecated compatibility alias for code that still includes the old
  // two-lane contract. New code must choose the narrower role explicitly.
  Safety = RuntimeSafety,
};

inline constexpr std::size_t kSchedulerPriorityCount = 6;

inline constexpr SchedulerPriority scheduler_priority_at(std::size_t index) {
  switch (index) {
    case 0:
      return SchedulerPriority::ControllerCommand;
    case 1:
      return SchedulerPriority::RuntimeSafety;
    case 2:
      return SchedulerPriority::StopControl;
    case 3:
      return SchedulerPriority::Lifecycle;
    case 4:
      return SchedulerPriority::Monitoring;
    default:
      return SchedulerPriority::Background;
  }
}

inline constexpr std::size_t scheduler_priority_index(
    SchedulerPriority priority) {
  switch (priority) {
    case SchedulerPriority::ControllerCommand:
      return 0;
    case SchedulerPriority::RuntimeSafety:
      return 1;
    case SchedulerPriority::StopControl:
      return 2;
    case SchedulerPriority::Lifecycle:
      return 3;
    case SchedulerPriority::Monitoring:
      return 4;
    case SchedulerPriority::Background:
      return 5;
  }
  return 5;
}

inline constexpr std::string_view scheduler_priority_name(
    SchedulerPriority priority) {
  switch (priority) {
    case SchedulerPriority::ControllerCommand:
      return "controller_command";
    case SchedulerPriority::RuntimeSafety:
      return "runtime_safety";
    case SchedulerPriority::StopControl:
      return "stop_control";
    case SchedulerPriority::Lifecycle:
      return "lifecycle";
    case SchedulerPriority::Monitoring:
      return "monitoring";
    case SchedulerPriority::Background:
      return "background";
  }
  return "unknown";
}

/// Abstract scheduler interface for deferred callbacks.
/// ADR-0004: no std::this_thread::sleep_for in core.
/// Production: backed by rclcpp timers or a timer thread.
/// Tests: use ManualScheduler for deterministic advancement.
class IScheduler {
 public:
  virtual ~IScheduler() = default;

  /// Schedule a callback to fire at or after the given deadline.
  /// Returns a handle that can be used to cancel.
  ///
  /// The `priority` hint selects which worker pool runs the callback
  /// in production schedulers — see `SchedulerPriority` for the
  /// threading invariants. Callers omitting the argument get `Lifecycle`;
  /// runtime-critical callbacks must select their narrower role explicitly.
  virtual ScheduleHandle schedule(
      TimePoint deadline, std::function<void()> callback,
      SchedulerPriority priority = SchedulerPriority::Lifecycle) = 0;

  /// Cancel a previously scheduled callback. No-op if already fired or invalid.
  virtual void cancel(ScheduleHandle handle) = 0;

  /// Wait until callbacks already dequeued on the given priority lane have
  /// returned. Test schedulers that invoke callbacks synchronously may keep the
  /// default no-op implementation.
  virtual void drain(SchedulerPriority /*priority*/) {}

  /// Bounded form of drain(). Returns false when callbacks already dequeued on
  /// the given lane did not return before the timeout. Synchronous test
  /// schedulers may keep the default no-op implementation.
  virtual bool drain_for(SchedulerPriority priority,
                         std::chrono::nanoseconds /*timeout*/) {
    drain(priority);
    return true;
  }

  /// Drain every priority lane known to the scheduler.
  virtual void drain_all() {
    for (std::size_t i = 0; i < kSchedulerPriorityCount; ++i) {
      drain(scheduler_priority_at(i));
    }
  }

  /// Bounded form of drain_all().
  virtual bool drain_all_for(std::chrono::nanoseconds timeout) {
    bool drained = true;
    for (std::size_t i = 0; i < kSchedulerPriorityCount; ++i) {
      drained = drain_for(scheduler_priority_at(i), timeout) && drained;
    }
    return drained;
  }
};

}  // namespace manyforge::core
