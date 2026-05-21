#pragma once

#include <cstdint>
#include <functional>

#include <manyforge_core/time/iclock.hpp>

namespace manyforge::core {

/// Handle for a scheduled callback. Used to cancel.
using ScheduleHandle = uint64_t;

/// Worker-pool affinity hint for scheduled callbacks.
///
/// Production schedulers (SimpleScheduler) MUST keep priorities on
/// independent worker threads so that a `Safety` callback blocked on
/// a long-running scene/planner lock cannot starve a `Monitoring`
/// watchdog whose job is to abort that very stall.
///
/// Threading invariant for callbacks (also documented in spec/220):
/// - `Safety` callbacks (collision monitor, soft-stop deceleration,
///   lifecycle bookkeeping) MAY take blocking locks; they SHOULD be
///   bounded in their critical work and must release locks promptly.
/// - `Monitoring` callbacks (planning timeouts, health probes, idle
///   watchdogs) MUST NOT take blocking locks on the planning/scene
///   path. They exist precisely so that the safety lane stalling
///   does not also stall the lane that aborts the stall.
///
/// Test schedulers (ManualScheduler) and adapters that single-thread
/// callbacks SHOULD treat the value as advisory.
enum class SchedulerPriority {
  Safety = 0,
  Monitoring = 1,
};

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
  /// threading invariants. Callers omitting the argument get
  /// `Safety` to keep existing behaviour (every callback on the
  /// safety lane); only watchdog-style timeouts and other non-
  /// blocking monitoring callbacks should opt in to `Monitoring`.
  virtual ScheduleHandle schedule(
      TimePoint deadline, std::function<void()> callback,
      SchedulerPriority priority = SchedulerPriority::Safety) = 0;

  /// Cancel a previously scheduled callback. No-op if already fired or invalid.
  virtual void cancel(ScheduleHandle handle) = 0;
};

}  // namespace manyforge::core
