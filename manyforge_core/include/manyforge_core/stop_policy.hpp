#pragma once

#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/error_code.hpp>

#include <functional>
#include <mutex>

namespace manyforge::core {

/// Stop types per ADR-0005.
/// Soft: controller decelerates over a duration, then stops.
/// Hard: controller halts immediately.
enum class StopType { kSoft, kHard };

/// Policy layer for stop/pause/resume operations.
/// Wraps the ExecutionStateMachine and adds higher-level semantics:
///   - Soft stop with scheduler-based deceleration timing
///   - Hard stop with immediate halt (cancels pending soft stop)
///   - Cache invalidation on any stop
///   - Thread-safe stop request tracking
/// ADR-0005: Kernel owns stop/pause semantics.
class StopPolicy {
 public:
  using CacheInvalidationCallback = std::function<void()>;
  using StopCallback = std::function<void()>;

  /// @param sm The state machine to drive.
  /// @param scheduler Scheduler for deferred soft-stop completion.
  /// @param clock Clock for computing deceleration deadlines.
  /// @param on_invalidate Called when cache should be invalidated (on any stop).
  StopPolicy(ExecutionStateMachine& sm, IScheduler& scheduler, IClock& clock,
             CacheInvalidationCallback on_invalidate = nullptr);

  /// Request a soft stop with deceleration.
  /// From kExecuting: transitions to kStopping, schedules deferred kStopped.
  /// From kPaused: transitions to kStopping then kStopped immediately
  ///   (no deceleration needed when already stationary).
  /// Triggers cache invalidation.
  /// @param deceleration_ns Duration before completing the stop.
  /// @return Ok on success, Err if transition is invalid.
  Result<void> request_soft_stop(Duration deceleration_ns);

  /// Request a hard stop (immediate halt).
  /// Cancels any pending soft stop. Transitions to kStopping then kStopped.
  /// Also valid from kStopping (to upgrade a soft stop to hard).
  /// Triggers cache invalidation.
  /// @return Ok on success, Err if transition is invalid.
  Result<void> request_hard_stop();

  /// Request pause (kExecuting -> kPaused).
  /// @return Ok on success, Err if transition is invalid.
  Result<void> request_pause();

  /// Resume from pause (kPaused -> kExecuting).
  /// @return Ok on success, Err if transition is invalid.
  Result<void> request_resume();

  /// Set a callback fired when any stop completes (soft or hard).
  void set_stop_callback(StopCallback callback);

  /// Check if a stop has been requested (kStopping or kStopped).
  bool stop_requested() const;

 private:
  ExecutionStateMachine& sm_;
  IScheduler& scheduler_;
  IClock& clock_;
  CacheInvalidationCallback on_invalidate_;
  StopCallback stop_callback_;
  mutable std::mutex mutex_;
  bool stop_requested_{false};
  ScheduleHandle pending_soft_stop_{0};
};

}  // namespace manyforge::core
