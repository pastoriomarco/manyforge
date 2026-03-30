#pragma once

#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/error_code.hpp>

#include <functional>
#include <mutex>

namespace manyforge::core {

enum class StopType { kSoft, kHard };

/// Policy layer for stop/pause/resume/reset operations.
class StopPolicy {
 public:
  using CacheInvalidationCallback = std::function<void()>;
  using StopCallback = std::function<void()>;

  StopPolicy(ExecutionStateMachine& sm, IScheduler& scheduler, IClock& clock,
             CacheInvalidationCallback on_invalidate = nullptr);

  /// Soft stop with deceleration.
  /// From Executing: Executing → Stopping, schedules deferred Stopped.
  /// From Paused: Paused → Stopping → Stopped immediately.
  /// From Planning/Validating: direct → Stopped.
  Result<void> request_soft_stop(Duration deceleration_ns);

  /// Hard stop (immediate halt). Cancels pending soft stop.
  /// From Executing/Paused: → Stopping → Stopped.
  /// From Stopping: upgrades soft → hard (immediate Stopped).
  /// From Planning/Validating: direct → Stopped.
  Result<void> request_hard_stop();

  /// Pause. Only from Executing.
  /// Executing → Stopping → Paused (controlled deceleration via scheduler).
  Result<void> request_pause();

  /// Resume. Paused → Validating (re-validates before resuming).
  Result<void> request_resume();

  /// Reset to Idle. Valid from any non-Idle state.
  /// From terminal states: direct → Idle.
  /// From active states: stops first, then → Idle.
  Result<void> request_reset();

  void set_stop_callback(StopCallback callback);
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
